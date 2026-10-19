cmake_minimum_required(VERSION 3.28)
project(netplay_kit VERSION 0.0.1 LANGUAGES CXX)

# A reusable C++ framework for online multiplayer games: transport, rollback netcode, sessions, lobby and home page.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Tests and sanitizers are this repository's own business: a game that pulls
# netplay-kit in with FetchContent gets neither unless it asks.
option(NPK_BUILD_TESTS "Build netplay-kit's tests" ${PROJECT_IS_TOP_LEVEL})
option(NPK_SANITIZE "Build Debug with AddressSanitizer and UBSan" ${PROJECT_IS_TOP_LEVEL})
if(NPK_SANITIZE AND CMAKE_BUILD_TYPE STREQUAL "Debug"
   AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(netplay_kit
  src/version.cpp
  src/transport/address.cpp
  src/transport/client.cpp
  src/transport/server.cpp
  src/transport/siphash.cpp
  src/transport/sim_link.cpp
  src/transport/udp_socket.cpp
  src/transport/wire.cpp)
add_library(npk::netplay_kit ALIAS netplay_kit)
target_include_directories(netplay_kit PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(netplay_kit PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(NPK_BUILD_TESTS)
  enable_testing()
  function(npk_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE npk::netplay_kit)
    target_include_directories(${name} PRIVATE tests)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME ${name} COMMAND ${name})
  endfunction()

  npk_test(smoke_test)
  npk_test(siphash_test)
  npk_test(wire_test)
  npk_test(wire_fuzz_test)
  npk_test(sim_link_test)
  npk_test(udp_loopback_test)
  npk_test(handshake_test)
endif()
