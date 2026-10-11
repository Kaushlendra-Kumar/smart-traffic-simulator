cmake_minimum_required(VERSION 3.20)
project(smart_traffic_simulator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(TRAFFIC_BUILD_TESTS      "Build GoogleTest suites"            ON)
option(TRAFFIC_BUILD_BENCHMARKS "Build Google Benchmark suites"      ON)
option(TRAFFIC_BUILD_SERVER     "Build the REST/WebSocket server"    ON)
option(TRAFFIC_FETCH_DEPS       "Download missing deps with FetchContent" ON)
set(TRAFFIC_SANITIZE "" CACHE STRING "Sanitizers: '', 'address,undefined' or 'thread'")

# ---------------------------------------------------------------- sanitizers
if(TRAFFIC_SANITIZE)
  if(MSVC)
    message(WARNING "TRAFFIC_SANITIZE is only wired up for GCC/Clang; ignoring.")
  else()
    message(STATUS "Sanitizers enabled: ${TRAFFIC_SANITIZE}")
    add_compile_options(-fsanitize=${TRAFFIC_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${TRAFFIC_SANITIZE})
  endif()
endif()

function(traffic_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
  endif()
endfunction()

# ---------------------------------------------------------------- dependencies
include(FetchContent)
find_package(Threads REQUIRED)

find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
  if(NOT TRAFFIC_FETCH_DEPS)
    message(FATAL_ERROR "nlohmann_json not found and TRAFFIC_FETCH_DEPS=OFF")
  endif()
  FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
  FetchContent_MakeAvailable(json)
endif()

if(TRAFFIC_BUILD_SERVER)
  find_path(ASIO_INCLUDE_DIR asio.hpp)
  if(NOT ASIO_INCLUDE_DIR)
    if(NOT TRAFFIC_FETCH_DEPS)
      message(FATAL_ERROR "standalone Asio not found and TRAFFIC_FETCH_DEPS=OFF")
    endif()
    FetchContent_Declare(asio GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git GIT_TAG asio-1-30-2 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(asio)
    set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include)
  endif()
  add_library(asio_headers INTERFACE)
  target_include_directories(asio_headers SYSTEM INTERFACE ${ASIO_INCLUDE_DIR})
  target_compile_definitions(asio_headers INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
  target_link_libraries(asio_headers INTERFACE Threads::Threads)

  find_package(spdlog QUIET)
  if(NOT spdlog_FOUND)
    if(NOT TRAFFIC_FETCH_DEPS)
      message(FATAL_ERROR "spdlog not found and TRAFFIC_FETCH_DEPS=OFF")
    endif()
    FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.12.0 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(spdlog)
  endif()
endif()

# ---------------------------------------------------------------- engine (no third-party deps)
add_library(traffic_engine
  src/core/geometry.cpp
  src/core/simulation_engine.cpp
  src/core/commands.cpp
  src/network/road_network.cpp
  src/network/network_builder.cpp
  src/network/routing.cpp
  src/vehicles/idm.cpp
  src/vehicles/vehicle_spawner.cpp
  src/signals/traffic_signal.cpp
  src/signals/phase_builder.cpp
  src/pedestrians/pedestrian_model.cpp
  src/controllers/fixed_time_controller.cpp
  src/controllers/adaptive_controller.cpp
  src/controllers/max_pressure_controller.cpp
  src/controllers/controller_factory.cpp
  src/metrics/metrics_collector.cpp
  src/metrics/statistics.cpp
  src/experiments/experiment_runner.cpp
)
target_include_directories(traffic_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(traffic_engine PUBLIC Threads::Threads)
traffic_warnings(traffic_engine)

# ---------------------------------------------------------------- infrastructure (JSON, coordinator)
add_library(traffic_infra
  src/infrastructure/scenario_io.cpp
  src/infrastructure/snapshot_json.cpp
  src/infrastructure/command_parser.cpp
  src/infrastructure/simulation_coordinator.cpp
  src/infrastructure/experiment_io.cpp
)
target_link_libraries(traffic_infra PUBLIC traffic_engine nlohmann_json::nlohmann_json)
traffic_warnings(traffic_infra)

# ---------------------------------------------------------------- headless CLI
add_executable(traffic_cli src/cli/cli_main.cpp)
target_link_libraries(traffic_cli PRIVATE traffic_infra)
traffic_warnings(traffic_cli)

# ---------------------------------------------------------------- server
if(TRAFFIC_BUILD_SERVER)
  add_library(traffic_api
    src/api/sha1.cpp
    src/api/http.cpp
    src/api/websocket.cpp
    src/api/server.cpp
    src/api/routes.cpp
    src/api/scenario_manager.cpp
    src/api/experiment_manager.cpp
  )
  target_include_directories(traffic_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(traffic_api PUBLIC traffic_infra asio_headers spdlog::spdlog)
  traffic_warnings(traffic_api)

  add_executable(traffic_server src/main.cpp)
  target_link_libraries(traffic_server PRIVATE traffic_api)
  traffic_warnings(traffic_server)
endif()

# ---------------------------------------------------------------- tests & benchmarks
if(TRAFFIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(TRAFFIC_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
