cmake_minimum_required(VERSION 3.14)
project(calltrace CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Both tools share common.h — no separate library needed since it's header-only.
# If common.h ever grows a .cpp, add it here as a static library.

add_executable(calltrace calltrace.cpp)
add_executable(functions functions.cpp)

# Reasonable default flags; override with -DCMAKE_BUILD_TYPE=Debug etc.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

target_compile_options(calltrace PRIVATE -Wall -Wextra)
target_compile_options(functions PRIVATE -Wall -Wextra)

install(TARGETS calltrace functions DESTINATION bin)
