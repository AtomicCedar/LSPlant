#include "utils/hook_helper.hpp"

#include <cassert>
#include <chrono>
#include <future>
#include <latch>

using namespace lsplant;
using namespace std::chrono_literals;

namespace {
std::atomic<int> callback_calls;
std::atomic<int> backup_calls;
int callback_dependency;

int Original(int value) {
    ++backup_calls;
    return value + 1;
}

constexpr auto hook = "test_function"_sym.hook->*[]<Backup auto backup>(int value) static -> int {
    ++callback_calls;
    assert(callback_dependency == 42);
    return backup(value) + callback_dependency;
};

struct Object {
    int value;
};

int OriginalMember(Object *object, int value) {
    ++backup_calls;
    return object->value + value;
}

constexpr auto member_hook =
    "test_member"_sym.hook->*[]<MemBackup auto backup>(Object *object, int value) static -> int {
    ++callback_calls;
    assert(callback_dependency == 42);
    return backup(object, value) + callback_dependency;
};

void OriginalVoid(int &value) {
    ++backup_calls;
    ++value;
}

constexpr auto void_hook =
    "test_void"_sym.hook->*[]<Backup auto backup>(int &value) static -> void {
    ++callback_calls;
    assert(callback_dependency == 42);
    backup(value);
};

void TestPublication(bool success) {
    callback_calls = 0;
    backup_calls = 0;
    callback_dependency = 0;
    hook = nullptr;
    member_hook = nullptr;
    void_hook = nullptr;

    std::array<std::future<int>, 4> pending;
    int (*replacement)(int) = nullptr;
    int (*member_replacement)(Object *, int) = nullptr;
    void (*void_replacement)(int &) = nullptr;
    Object object{10};
    auto initialize = [&] {
        HookHandler::InitScope scope;
        InitInfo info;
        info.inline_hooker = [&](void *original, void *replace) -> void * {
            if (original == reinterpret_cast<void *>(Original)) {
                replacement = reinterpret_cast<decltype(replacement)>(replace);
                std::latch started{pending.size()};
                for (auto &call : pending) {
                    call = std::async(std::launch::async, [&] {
                        started.count_down();
                        return replacement(3);
                    });
                }
                started.wait();
                // Simulate an entry patch visible before inline_hooker returns a backup.
                for (auto &call : pending) {
                    assert(call.wait_for(50ms) == std::future_status::timeout);
                }
                assert(callback_calls == 0);
                assert(backup_calls == 0);
            } else if (original == reinterpret_cast<void *>(OriginalMember)) {
                member_replacement = reinterpret_cast<decltype(member_replacement)>(replace);
            } else {
                void_replacement = reinterpret_cast<decltype(void_replacement)>(replace);
            }
            return original;
        };
        info.art_symbol_resolver = [](std::string_view symbol) -> void * {
            if (symbol == "test_function") return reinterpret_cast<void *>(Original);
            if (symbol == "test_member") return reinterpret_cast<void *>(OriginalMember);
            if (symbol == "test_void") return reinterpret_cast<void *>(OriginalVoid);
            return nullptr;
        };
        HookHandler handler{info};
        assert(handler(hook));
        assert(handler(member_hook));
        assert(handler(void_hook));

        // Publishing one backup must not release callbacks before their dependencies are ready.
        for (auto &call : pending) {
            assert(call.wait_for(50ms) == std::future_status::timeout);
        }
        assert(callback_calls == 0);
        assert(backup_calls == 0);

        callback_dependency = 42;

        // Init's own JNI work executes callbacks without waiting for itself.
        assert(replacement(3) == 46);
        assert(member_replacement(&object, 3) == 55);
        int value = 0;
        void_replacement(value);
        assert(value == 1);
        // Skipping the wait on this thread must not release any other thread.
        for (auto &call : pending) {
            assert(call.wait_for(50ms) == std::future_status::timeout);
        }
        assert(callback_calls == 3);

        // The barrier publishes completion regardless of the caller's success result.
        return success;
    };
    assert(initialize() == success);

    // Every waiter must be notified on both success and failure.
    for (auto &call : pending) {
        assert(call.wait_for(5s) == std::future_status::ready);
        assert(call.get() == 46);
    }
    assert(member_replacement(&object, 3) == 55);
    int value = 0;
    void_replacement(value);
    assert(value == 1);
    assert(callback_calls == pending.size() + 5);
    assert(backup_calls == pending.size() + 5);
}
}  // namespace

int main() {
    TestPublication(true);
    TestPublication(false);
}
