# Host unit tests for the header-only logic in main/ and components/ — no
# board, no ESP-IDF:
#   cmake -S test -B build-test -G Ninja && cmake --build build-test
#   ctest --test-dir build-test --output-on-failure        (task test)
cmake_minimum_required(VERSION 3.16)
project(wifi_camera_tests CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS ON) # gnu++17, as before

include(FetchContent)
FetchContent_Declare(unity
    URL https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v2.6.1.tar.gz
    URL_HASH SHA256=b41a66d45a6b99758fb3202ace6178177014d52fc524bf1f72687d93e9867292
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(unity)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)

enable_testing()
set(root ${CMAKE_CURRENT_SOURCE_DIR}/..)
foreach(name test_token_rules test_strbuf test_vqf test_mov test_detect test_ov5640)
    add_executable(${name} ${name}/test_main.cpp)
    target_include_directories(${name} PRIVATE ${root}/main ${root}/components/VQF/include)
    target_link_libraries(${name} PRIVATE unity)
    add_test(NAME ${name} COMMAND ${name})
endforeach()
