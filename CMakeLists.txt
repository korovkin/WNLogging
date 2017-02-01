cmake_minimum_required(VERSION 3.2)
project (wnlogging)

add_definitions(-std=c++11)
add_definitions(-Wall)

set(SOURCES
  main.cpp
  WNLogging.cpp
)

set(HEADERS
  WNLogging.h
)

add_executable(
  wnlogging
  ${SOURCES}
  ${HEADERS}
)

target_include_directories(
  wnlogging SYSTEM
  PUBLIC /usr/local/include
)

target_link_libraries(
  wnlogging
  /usr/local/lib/libgflags.a
)

