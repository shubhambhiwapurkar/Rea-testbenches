# Standalone build of the engine-independent simulation core, its tests and the headless CLI.
# Unreal builds the same sources through Source/SummitSim/SummitSim.Build.cs.
cmake_minimum_required(VERSION 3.20)
project(SummitSim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Match Unreal's compile environment: no exceptions, no RTTI.
if(MSVC)
  add_compile_options(/W4 /permissive- /GR- /EHs-c-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -fno-exceptions -fno-rtti)
endif()

file(GLOB SUMMIT_SIM_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/Source/SummitSim/Private/Sim/*.cpp)
add_library(summit_sim STATIC ${SUMMIT_SIM_SOURCES})
target_include_directories(summit_sim PUBLIC ${CMAKE_SOURCE_DIR}/Source/SummitSim/Public)

file(GLOB SUMMIT_SIM_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/Tests/Sim/*.cpp)
add_executable(summit_sim_tests ${SUMMIT_SIM_TESTS})
target_link_libraries(summit_sim_tests PRIVATE summit_sim)

add_executable(summit_sim_cli ${CMAKE_SOURCE_DIR}/Tools/cli/SummitSimCli.cpp)
target_link_libraries(summit_sim_cli PRIVATE summit_sim)

enable_testing()
add_test(NAME summit_sim_tests COMMAND summit_sim_tests)
