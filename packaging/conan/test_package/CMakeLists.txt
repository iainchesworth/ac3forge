cmake_minimum_required(VERSION 3.28)
project(test_ac3forge CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ac3forge CONFIG REQUIRED)

add_executable(test_ac3forge src/test_ac3forge.cpp)
target_link_libraries(test_ac3forge PRIVATE ac3::forge)
