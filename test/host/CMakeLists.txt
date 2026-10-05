cmake_minimum_required(VERSION 3.28)
project(lsplant_hook_helper_test LANGUAGES CXX)

find_package(JNI REQUIRED)
find_package(Threads REQUIRED)

add_executable(hook_helper_test hook_helper_test.cpp)
target_compile_features(hook_helper_test PRIVATE cxx_std_23)
target_include_directories(hook_helper_test PRIVATE
    ${JNI_INCLUDE_DIRS} ../../lsplant/src/main/jni/include)
target_compile_options(hook_helper_test PRIVATE
    -Wall -Wextra -Werror -Wno-gnu-string-literal-operator-template -UNDEBUG)
target_link_libraries(hook_helper_test PRIVATE Threads::Threads)

enable_testing()
add_test(NAME hook_helper_test COMMAND hook_helper_test)
set_tests_properties(hook_helper_test PROPERTIES TIMEOUT 30)
