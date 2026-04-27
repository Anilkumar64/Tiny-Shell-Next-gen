cmake_minimum_required(VERSION 3.20)
project(TinyShell LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenSSL
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Feature 7: Binary Diversity Engine (Run Mutator)
execute_process(COMMAND ${CMAKE_SOURCE_DIR}/scripts/mutate.sh)

add_library(common
    common/SecureChannel.cpp
)

target_include_directories(common PUBLIC 
    common
    ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(common PUBLIC 
    OpenSSL::Crypto
    Threads::Threads
)

add_executable(tsh_client
    client/main.cpp
    client/Client.cpp
)

target_link_libraries(tsh_client common)

add_executable(tsh_server
    server/main.cpp
    server/Server.cpp
)

target_link_libraries(tsh_server common)
