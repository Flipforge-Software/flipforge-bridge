cmake_minimum_required(VERSION 3.16)
project(flipforge_bridge_host_tests C)

enable_testing()
find_package(OpenSSL REQUIRED)

add_executable(
    flipforge_bridge_tests
    test_main.c
    ../main/bridge_auth.c
    ../main/bridge_io.c
    ../main/bridge_session.c
    ../main/expansion_protocol.c
    ../main/expansion_state_machine.c
)

target_include_directories(flipforge_bridge_tests PRIVATE ../main)
target_compile_features(flipforge_bridge_tests PRIVATE c_std_11)
target_compile_options(flipforge_bridge_tests PRIVATE -Wall -Wextra -Werror -pedantic)
target_compile_definitions(flipforge_bridge_tests PRIVATE BRIDGE_HOST_TEST=1)
target_link_libraries(flipforge_bridge_tests PRIVATE OpenSSL::Crypto)

add_test(NAME flipforge_bridge_tests COMMAND flipforge_bridge_tests)
