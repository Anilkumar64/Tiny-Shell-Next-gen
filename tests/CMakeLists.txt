find_package(GTest REQUIRED)

add_executable(tsh_tests
    test_agent_identity.cpp
    test_ast_signer.cpp
    test_cluster_health.cpp
    test_controller_identity.cpp
    test_http_api.cpp
    test_parser.cpp
    test_pipeline_executor.cpp
    test_proc_reader.cpp
    test_rbac_multitenancy.cpp
    test_sandbox_executor.cpp
    test_secure_channel.cpp
    ${CMAKE_SOURCE_DIR}/spine/agent/SandboxExecutor.cpp
)

target_include_directories(tsh_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/common/transport
    ${CMAKE_SOURCE_DIR}/server
    ${CMAKE_SOURCE_DIR}/spine/agent
    ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(tsh_tests PRIVATE
    GTest::gtest_main
    tsh_security
    tsh_infrastructure
    tsh_server_runtime
    tsh_spine_proto
    tsh_spine_common   # FIX (Bug 3): SandboxExecutor.cpp compiled directly into
                       # tsh_tests needs the spine/common signing + policy symbols.
    OpenSSL::Crypto
    Threads::Threads
)

tsh_harden(tsh_tests)

include(GoogleTest)
gtest_discover_tests(tsh_tests)