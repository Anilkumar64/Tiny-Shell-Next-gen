cmake_minimum_required(VERSION 3.20)
project(TinyShell LANGUAGES CXX)

# ── Language standard ─────────────────────────────────────────────────────────
# FIX: was CMAKE_CXX_STANDARD 20.  Spec (Section 7) mandates C++17.
# The one C++20 call in the codebase (unordered_set::contains()) has been
# replaced with find() != end() in SecureChannel.cpp.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sanitizers are opt-in via -DTSH_SANITIZER_FLAGS="-fsanitize=address,undefined"
if(NOT DEFINED TSH_SANITIZER_FLAGS)
    set(TSH_SANITIZER_FLAGS "")
endif()

function(tsh_harden target)
    target_compile_options(${target} PRIVATE -Wall -Wextra ${TSH_SANITIZER_FLAGS})
    if(NOT "${TSH_SANITIZER_FLAGS}" STREQUAL "")
        target_link_options(${target} PRIVATE ${TSH_SANITIZER_FLAGS})
    endif()
endfunction()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_program(PROTOC_EXECUTABLE protoc PATHS /usr/bin NO_DEFAULT_PATH REQUIRED)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin PATHS /usr/bin
             NO_DEFAULT_PATH REQUIRED)
set(Protobuf_PROTOC_EXECUTABLE ${PROTOC_EXECUTABLE} CACHE FILEPATH "" FORCE)
set(Protobuf_INCLUDE_DIR /usr/include CACHE PATH "" FORCE)
set(Protobuf_LIBRARY_RELEASE /usr/lib/x86_64-linux-gnu/libprotobuf.so CACHE FILEPATH "" FORCE)
set(Protobuf_LIBRARY_DEBUG /usr/lib/x86_64-linux-gnu/libprotobuf.so CACHE FILEPATH "" FORCE)
find_package(Protobuf REQUIRED)
find_library(GRPCPP_LIBRARY grpc++ PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH REQUIRED)
find_library(GRPC_LIBRARY grpc PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH REQUIRED)
find_library(GPR_LIBRARY gpr PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH REQUIRED)

pkg_check_modules(UUID uuid)
if(NOT UUID_FOUND)
    message(WARNING "UUID library not found - multi-tenancy features may not link.")
endif()

# ---------------------------------------------------------------------------
# tsh_security  -  SecureChannel (TLS/mTLS over TCP)
# ---------------------------------------------------------------------------
add_library(tsh_security STATIC
    common/SecureChannel.cpp
)
target_include_directories(tsh_security PUBLIC
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/common/transport
    ${CMAKE_SOURCE_DIR}/config
    ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(tsh_security PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
tsh_harden(tsh_security)

# ---------------------------------------------------------------------------
# tsh_infrastructure  -  OS primitives (/proc reader, etc.)
# ---------------------------------------------------------------------------
add_library(tsh_infrastructure STATIC
    common/ProcReader.cpp
)
target_include_directories(tsh_infrastructure PUBLIC
    ${CMAKE_SOURCE_DIR}/common
)
tsh_harden(tsh_infrastructure)

# ---------------------------------------------------------------------------
# tsh_spine_proto  -  gRPC/Protobuf contracts for the real execution spine
# ---------------------------------------------------------------------------
set(TSH_PROTO_FILE ${CMAKE_SOURCE_DIR}/proto/tinyshell/v1/spine.proto)
set(TSH_PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
set(TSH_PROTO_SRCS
    ${TSH_PROTO_GEN_DIR}/tinyshell/v1/spine.pb.cc
    ${TSH_PROTO_GEN_DIR}/tinyshell/v1/spine.grpc.pb.cc
)
set(TSH_PROTO_HDRS
    ${TSH_PROTO_GEN_DIR}/tinyshell/v1/spine.pb.h
    ${TSH_PROTO_GEN_DIR}/tinyshell/v1/spine.grpc.pb.h
)

add_custom_command(
    OUTPUT ${TSH_PROTO_SRCS} ${TSH_PROTO_HDRS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TSH_PROTO_GEN_DIR}
    COMMAND ${PROTOC_EXECUTABLE}
            --proto_path=${CMAKE_SOURCE_DIR}/proto
            --cpp_out=${TSH_PROTO_GEN_DIR}
            --grpc_out=${TSH_PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
            ${TSH_PROTO_FILE}
    DEPENDS ${TSH_PROTO_FILE}
    COMMENT "Generating TinyShell execution spine protobufs"
)

add_library(tsh_spine_proto STATIC
    ${TSH_PROTO_SRCS}
    ${TSH_PROTO_HDRS}
)
target_include_directories(tsh_spine_proto PUBLIC
    ${TSH_PROTO_GEN_DIR}
    ${Protobuf_INCLUDE_DIRS}
)
target_link_libraries(tsh_spine_proto PUBLIC
    ${Protobuf_LIBRARIES}
    ${GRPCPP_LIBRARY}
    ${GRPC_LIBRARY}
    ${GPR_LIBRARY}
    Threads::Threads
)
tsh_harden(tsh_spine_proto)

# ---------------------------------------------------------------------------
# tsh_spine_common  -  shared signing and policy code
# ---------------------------------------------------------------------------
add_library(tsh_spine_common STATIC
    spine/common/CommandPolicy.cpp
    spine/common/JobSigner.cpp
)
target_include_directories(tsh_spine_common PUBLIC
    ${CMAKE_SOURCE_DIR}/spine/common
    ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(tsh_spine_common PUBLIC
    tsh_spine_proto
    OpenSSL::Crypto
)
tsh_harden(tsh_spine_common)

# ---------------------------------------------------------------------------
# tsh_server_runtime  -  Server + all feature headers
# ---------------------------------------------------------------------------
add_library(tsh_server_runtime STATIC
    server/Server.cpp
    server/RemoteWorkerClient.cpp
)
target_include_directories(tsh_server_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/common/transport
    ${CMAKE_SOURCE_DIR}/server
    ${CMAKE_SOURCE_DIR}/config
    ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(tsh_server_runtime PUBLIC
    tsh_security
    tsh_infrastructure
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
if(UUID_FOUND)
    target_link_libraries(tsh_server_runtime PUBLIC ${UUID_LIBRARIES})
    target_include_directories(tsh_server_runtime PUBLIC ${UUID_INCLUDE_DIRS})
endif()
tsh_harden(tsh_server_runtime)

# ---------------------------------------------------------------------------
# tsh_client_runtime  -  Client logic
# ---------------------------------------------------------------------------
add_library(tsh_client_runtime STATIC
    client/Client.cpp
)
target_include_directories(tsh_client_runtime PUBLIC
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/common/transport
    ${CMAKE_SOURCE_DIR}/client
    ${CMAKE_SOURCE_DIR}/config
    ${OPENSSL_INCLUDE_DIR}
)
target_link_libraries(tsh_client_runtime PUBLIC
    tsh_security
)
tsh_harden(tsh_client_runtime)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(tsh_server server/main.cpp)
target_include_directories(tsh_server PRIVATE
    ${CMAKE_SOURCE_DIR}/server
    ${CMAKE_SOURCE_DIR}/config
)
target_link_libraries(tsh_server PRIVATE tsh_server_runtime)
tsh_harden(tsh_server)

add_executable(tsh_client client/main.cpp)
target_include_directories(tsh_client PRIVATE
    ${CMAKE_SOURCE_DIR}/client
)
target_link_libraries(tsh_client PRIVATE tsh_client_runtime)
tsh_harden(tsh_client)

add_executable(tsh_worker worker/main.cpp)
target_include_directories(tsh_worker PRIVATE
    ${CMAKE_SOURCE_DIR}/worker
    ${CMAKE_SOURCE_DIR}/server
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/config
)
target_link_libraries(tsh_worker PRIVATE
    tsh_security
    tsh_infrastructure
    Threads::Threads
)
tsh_harden(tsh_worker)

add_executable(tsh_spine_server
    spine/server/SpineServer.cpp
    spine/server/EventBus.cpp
    spine/server/JobLifecycle.cpp
    spine/server/PostgresEventStore.cpp
)
target_include_directories(tsh_spine_server PRIVATE
    ${CMAKE_SOURCE_DIR}/spine/common
    ${CMAKE_SOURCE_DIR}/spine/server
)
target_link_libraries(tsh_spine_server PRIVATE
    tsh_spine_common
)
tsh_harden(tsh_spine_server)

add_executable(tsh_spine_agent
    spine/agent/SpineAgent.cpp
    spine/agent/SandboxExecutor.cpp
)
target_include_directories(tsh_spine_agent PRIVATE
    ${CMAKE_SOURCE_DIR}/spine/common
    ${CMAKE_SOURCE_DIR}/spine/agent
)
target_link_libraries(tsh_spine_agent PRIVATE
    tsh_spine_common
)
tsh_harden(tsh_spine_agent)

add_executable(tsh_spine_submit
    spine/client/SpineSubmit.cpp
)
target_include_directories(tsh_spine_submit PRIVATE
    ${CMAKE_SOURCE_DIR}/spine/grpc
)
target_link_libraries(tsh_spine_submit PRIVATE
    tsh_spine_proto
)
tsh_harden(tsh_spine_submit)

# ---------------------------------------------------------------------------
# GUI (Qt6/Qt5 control panel)
# ---------------------------------------------------------------------------
add_subdirectory(gui)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()