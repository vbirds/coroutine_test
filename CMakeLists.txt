cmake_minimum_required(VERSION 2.8)
project(coroutine)

set(CMAKE_CXX_STANDARD 11)

include_directories(./)

aux_source_directory(./common SRCS)

add_executable(coroutine main.cpp ${SRCS})
target_link_libraries(coroutine pthread)
