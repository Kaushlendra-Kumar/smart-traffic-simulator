find_package(GTest QUIET)
if(NOT GTest_FOUND)
  if(NOT TRAFFIC_FETCH_DEPS)
    message(FATAL_ERROR "GoogleTest not found and TRAFFIC_FETCH_DEPS=OFF")
  endif()
  FetchContent_Declare(googletest URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
include(GoogleTest)

add_executable(traffic_tests
  unit/test_geometry_stats.cpp
  unit/test_network.cpp
  unit/test_idm.cpp
  unit/test_signals.cpp
  unit/test_controllers.cpp
  unit/test_metrics.cpp
  unit/test_io.cpp
  integration/test_simulation.cpp
  integration/test_coordinator.cpp
  simulation/test_invariants.cpp
)
target_include_directories(traffic_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(traffic_tests PRIVATE traffic_infra GTest::gtest_main)
target_compile_definitions(traffic_tests PRIVATE TRAFFIC_SCENARIO_DIR="${PROJECT_SOURCE_DIR}/config/scenarios")
traffic_warnings(traffic_tests)
gtest_discover_tests(traffic_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 300)

if(TRAFFIC_BUILD_SERVER)
  add_executable(traffic_server_tests integration/test_server.cpp)
  target_include_directories(traffic_server_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(traffic_server_tests PRIVATE traffic_api GTest::gtest_main)
  target_compile_definitions(traffic_server_tests PRIVATE TRAFFIC_SCENARIO_DIR="${PROJECT_SOURCE_DIR}/config/scenarios")
  traffic_warnings(traffic_server_tests)
  gtest_discover_tests(traffic_server_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 300)
endif()
