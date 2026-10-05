#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <string_view>
#include <thread>
#include <type_traits>

#include "lsplant.hpp"
#include "type_traits.hpp"

namespace lsplant {

template <size_t N, char... Cs>
struct FixedString {
    static constexpr auto data = [] consteval {
        static constexpr auto kString = std::array{Cs...};
        return std::string_view{kString.data(), N};
    }();
};

template <typename T>
concept FuncType = std::is_function_v<T> || std::is_member_function_pointer_v<T>;

template <FixedString Sym, FuncType Signature>
struct Function {
    static_assert(std::is_function_v<Signature>);

    template <typename... Args>
    [[gnu::always_inline]] static decltype(auto) operator()(Args &&...args) {
        return inner_.function_(std::forward<Args>(args)...);
    }
    [[gnu::always_inline]] operator bool() const { return inner_.raw_function_ != nullptr; }
    [[gnu::always_inline]] auto operator&() const { return inner_.function_; }
    [[gnu::always_inline]] auto &operator=(void *function) const {
        inner_.raw_function_ = function;
        return *this;
    }

private:
    inline static union {
        std::add_pointer_t<Signature> function_;
        void *raw_function_ = nullptr;
    } inner_;

    static_assert(sizeof(inner_.function_) == sizeof(inner_.raw_function_));
};

template <class Derived, typename This, typename Ret, typename... Args>
struct BaseMemberFunction {
    [[gnu::always_inline]] static Ret operator()(This *thiz, Args... args) {
        return (reinterpret_cast<ThisType *>(thiz)->*Derived::inner_.function_)(
            std::forward<Args>(args)...);
    }
    [[gnu::always_inline]] operator bool() const {
        return Derived::inner_.raw_function_ != nullptr;
    }
    [[gnu::always_inline]] auto operator&() const {
        return reinterpret_cast<Ret (*)(This *, Args...)>(Derived::inner_.raw_function_);
    }

protected:
    [[gnu::always_inline]] auto &operator=(void *function) const {
        Derived::inner_.raw_function_ = function;
        return *this;
    }

    using ThisType = std::conditional_t<std::is_void_v<This>, Derived, This>;
    union InnerType {
        Ret (ThisType::*function_)(Args...) const;

        struct {
            void *raw_function_ = nullptr;
            [[maybe_unused]] std::ptrdiff_t adj = 0;
        };
    };

    static_assert(sizeof(InnerType::function_) ==
                  sizeof(InnerType::raw_function_) + sizeof(InnerType::adj));
};

template <FixedString Sym, class This, typename Ret, typename... Args>
struct Function<Sym, Ret (This::*)(Args...)>
    : BaseMemberFunction<Function<Sym, Ret (This::*)(Args...)>, This, Ret, Args...> {
    [[gnu::always_inline]] auto &operator=(void *function) const {
        Function::BaseMemberFunction::operator=(function);
        return *this;
    }

private:
    inline static Function::BaseMemberFunction::InnerType inner_;
    friend struct Function::BaseMemberFunction;
};

template <FixedString Sym, class This, typename Ret, typename... Args>
struct Function<Sym, Ret (This::*const)(Args...)>
    : BaseMemberFunction<Function<Sym, Ret (This::*const)(Args...)>, const This, Ret, Args...> {
    [[gnu::always_inline]] auto &operator=(void *function) const {
        Function::BaseMemberFunction::operator=(function);
        return *this;
    }

private:
    inline static Function::BaseMemberFunction::InnerType inner_;
    friend struct Function::BaseMemberFunction;
};

template <FixedString, typename T>
struct Field {
    [[gnu::always_inline]] T *operator->() const { return inner_.field_; }
    [[gnu::always_inline]] T &operator*() const { return *inner_.field_; }
    [[gnu::always_inline]] operator bool() const { return inner_.raw_field_ != nullptr; }
    [[gnu::always_inline]] auto operator&() const { return inner_.field_; }
    [[gnu::always_inline]] auto &operator=(void *field) const {
        inner_.raw_field_ = field;
        return *this;
    }

private:
    inline static union {
        void *raw_field_ = nullptr;
        T *field_;
    } inner_;

    static_assert(sizeof(inner_.field_) == sizeof(inner_.raw_field_));
};

template <FixedString Sym, FuncType Signature>
struct Hooker : Function<Sym, Signature> {
    [[gnu::always_inline]] auto &operator=(void *function) const {
        Hooker::Function::operator=(function);
        return *this;
    }

private:
    using Replacement = decltype(&std::declval<Function<Sym, Signature>>());
    consteval Hooker(Replacement replace) : replace_{replace} {};

    inline static void *address_ = nullptr;

    Replacement replace_;

    friend struct HookHandler;
    template <FixedString S>
    friend struct Symbol;
};

struct HookHandler {
private:
    inline static std::atomic<std::thread::id> initializing_thread_id_{};
    static_assert(std::atomic<std::thread::id>::is_always_lock_free);

    [[gnu::cold, gnu::noinline]] static void WaitForInitializationSlow(std::thread::id thread_id) {
        // Installing hooks can reenter an already installed callback on this thread.
        if (thread_id == std::this_thread::get_id()) return;
        do {
            initializing_thread_id_.wait(thread_id, std::memory_order_acquire);
            thread_id = initializing_thread_id_.load(std::memory_order_acquire);
        } while (thread_id != std::thread::id{});
    }

public:
    class InitScope {
    public:
        InitScope() {
            // Publish the owner before installing any hook callbacks.
            initializing_thread_id_.store(std::this_thread::get_id(), std::memory_order_relaxed);
        }
        InitScope(const InitScope &) = delete;
        InitScope &operator=(const InitScope &) = delete;
        ~InitScope() {
            // Publish initialization writes and wake waiters on every exit path.
            initializing_thread_id_.store(std::thread::id{}, std::memory_order_release);
            initializing_thread_id_.notify_all();
        }
    };

    [[gnu::always_inline]] static void WaitForInitialization() {
        auto thread_id = initializing_thread_id_.load(std::memory_order_acquire);
        if (thread_id != std::thread::id{}) [[unlikely]] {
            WaitForInitializationSlow(thread_id);
        }
    }

    HookHandler(const InitInfo &info) : info_(info) {}

    template <typename T>
    [[gnu::always_inline]] bool operator()(T &&arg) const {
        return handle(std::forward<T>(arg), false);
    }

    template <typename T1, typename T2, typename... U>
    [[gnu::always_inline]] bool operator()(T1 &&arg1, T2 &&arg2, U &&...args) const {
        if constexpr (std::is_same_v<T2, bool>) {
            return handle(std::forward<T1>(arg1), std::forward<T2>(arg2)) ||
                   this->operator()(std::forward<U>(args)...);
        } else {
            return handle(std::forward<T1>(arg1), false) ||
                   this->operator()(std::forward<T2>(arg2), std::forward<U>(args)...);
        }
    }

    template <FixedString Sym, typename... Us, template <FixedString, typename...> typename T>
        requires(requires { T<Sym, Us...>::replace_; })
    [[gnu::always_inline]] bool unhook(const T<Sym, Us...> &hooker) const {
        if (hooker.address_ && info_.inline_unhooker && info_.inline_unhooker(hooker.address_)) {
            hooker = nullptr;
            hooker.address_ = nullptr;
            return true;
        }
        return false;
    }

private:
    [[gnu::always_inline]] constexpr bool operator()() const { return false; }

    template <FixedString Sym, typename... Us, template <FixedString, typename...> typename T>
        requires(!requires { T<Sym, Us...>::replace_; })
    [[gnu::always_inline]] bool handle(const T<Sym, Us...> &target, bool match_prefix) const {
        return target = dlsym<Sym>(match_prefix);
    }

    template <FixedString Sym, typename... Us, template <FixedString, typename...> typename T>
        requires(requires { T<Sym, Us...>::replace_; })
    [[gnu::always_inline]] bool handle(const T<Sym, Us...> &hooker, bool match_prefix) const {
        return hooker = hook(hooker.address_ = dlsym<Sym>(match_prefix),
                             reinterpret_cast<void *>(hooker.replace_));
    }

    template <FixedString Sym>
    [[gnu::always_inline, nodiscard]] void *dlsym(bool match_prefix = false) const {
        if (auto match = info_.art_symbol_resolver(Sym.data)) {
            return match;
        }
        if (match_prefix && info_.art_symbol_prefix_resolver) [[likely]] {
            return info_.art_symbol_prefix_resolver(Sym.data);
        }
        return nullptr;
    }

    [[gnu::always_inline, nodiscard]] void *hook(void *original, void *replace) const {
        if (original) [[likely]] {
            return info_.inline_hooker(original, replace);
        }
        return nullptr;
    }

    const InitInfo &info_;
};

template <typename>
inline constexpr bool is_function_wrapper_v = false;

template <FixedString Sym, FuncType Signature>
inline constexpr bool is_function_wrapper_v<Function<Sym, Signature>> = true;

template <typename F>
concept Backup =
    std::is_function_v<std::remove_pointer_t<F>> || is_function_wrapper_v<std::remove_cv_t<F>>;

template <typename F>
concept MemBackup = std::is_member_function_pointer_v<std::remove_pointer_t<F>> || Backup<F>;

template <FixedString S>
struct Symbol {
    template <typename T>
    inline static decltype([] {
        if constexpr (FuncType<T>) {
            return Function<S, T>{};
        } else {
            return Field<S, T>{};
        }
    }()) as{};

    template <bool kWaitForInitialization>
    struct Hook {
        template <typename HookerType, typename F>
        static consteval auto MakeHooker() {
            using BackupType = typename HookerType::Function;
            constexpr auto replace = static_cast<decltype(HookerType::replace_)>(
                &F::template operator()<BackupType{}>);
            if constexpr (!kWaitForInitialization) {
                return HookerType{replace};
            } else {
                return []<typename Ret, typename... Args>(Ret (*)(Args...)) {
                    return HookerType{+[](Args... args) static -> Ret {
                        HookHandler::WaitForInitialization();
                        return F::template operator()<BackupType{}>(std::forward<Args>(args)...);
                    }};
                }(replace);
            }
        }

        template <typename F>
        consteval auto operator->*(F && /*unused*/) const {
            using Signature = decltype(F::template operator()<&decltype([] static {})::operator()>);
            if constexpr (requires { F::template operator()<&decltype([] {})::operator()>; }) {
                using HookerType =
                    Hooker<S, decltype([]<class This, typename Ret, typename... Args>(
                                           Ret (*)(This *, Args...)) -> Ret (This::*)(Args...) {
                               return {};
                           }.template operator()(std::declval<Signature>()))>;
                return MakeHooker<HookerType, F>();
            } else {
                using HookerType = Hooker<S, Signature>;
                return MakeHooker<HookerType, F>();
            }
        };
    };

    [[no_unique_address]] Hook<true> hook;
    // Custom ABI callbacks must provide the initialization barrier themselves.
    [[no_unique_address]] Hook<false> raw_hook;
};

template <typename T, T... Cs>
    requires(std::is_same_v<T, char>)
consteval auto operator""_sym() {
    return Symbol<FixedString<sizeof...(Cs), Cs..., T{}>{}>{};
}

template <FixedString S, FixedString P>
consteval auto operator|([[maybe_unused]] Symbol<S> lp32, [[maybe_unused]] Symbol<P> lp64) {
    if constexpr (is_arch_v<Arch::kLP64>) {
        return lp64;
    } else {
        return lp32;
    }
}
}  // namespace lsplant
