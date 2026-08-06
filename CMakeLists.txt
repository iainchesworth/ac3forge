cmake_minimum_required(VERSION 3.28)

project(ac3forge VERSION 0.1.0 LANGUAGES CXX)

# On MSVC this selects /std:c++latest — there is no /std:c++23 switch yet.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AC3FORGE_BUILD_TESTS "Build unit tests" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
endif()

add_library(ac3core STATIC
  src/ac3/core/exponents.cpp
  src/ac3/core/mdct.cpp
)
add_library(ac3::core ALIAS ac3core)
target_include_directories(ac3core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(ac3core PUBLIC cxx_std_23)

add_executable(ac3cli apps/ac3cli/main.cpp)
target_link_libraries(ac3cli PRIVATE ac3::core)

if(AC3FORGE_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)
  add_executable(ac3tests
    tests/msvc_crt_report.cpp
    tests/test_bitwriter.cpp
    tests/test_crc16.cpp
    tests/test_exponents.cpp
    tests/test_frame.cpp
    tests/test_mdct.cpp
    tests/test_tables.cpp
  )
  target_link_libraries(ac3tests PRIVATE ac3::core Catch2::Catch2WithMain)
  target_include_directories(ac3tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  include(Catch)
  catch_discover_tests(ac3tests)
endif()
