cmake_minimum_required(VERSION 3.28)
project(netplay_kit VERSION 0.0.1 LANGUAGES CXX)

# A reusable C++ framework for online multiplayer games: transport, rollback netcode, sessions, lobby and home page.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(netplay_kit src/version.cpp)
add_library(npk::netplay_kit ALIAS netplay_kit)
target_include_directories(netplay_kit PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(netplay_kit PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(netplay_kit_smoke_test tests/smoke_test.cpp)
  target_link_libraries(netplay_kit_smoke_test PRIVATE npk::netplay_kit)
  add_test(NAME netplay_kit_smoke COMMAND netplay_kit_smoke_test)
endif()
