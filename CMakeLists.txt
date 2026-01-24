# Specify the minimum required version of CMake
cmake_minimum_required(VERSION 3.14)

# Specify the C++ standard to use
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Define the project name and enable C language
project(httpserver LANGUAGES C)

# Find all source files in the "src" directory
file(GLOB SOURCES "src/*.c")

# Libraries within the project
file(GLOB CJSON "src/lib/cJSON.c")

# Find zlib
find_package(ZLIB REQUIRED)

# Add an executable target named "httpserver" from the source files in the "src" directory
add_executable(httpserver ${SOURCES} ${CJSON})
target_link_libraries(httpserver PRIVATE ZLIB::ZLIB)

# Link pthread on MinGW/GCC Windows builds
if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_link_libraries(httpserver PRIVATE pthread)
endif()

# Link ws2_32 for Windows socket support
if(WIN32)
    target_link_libraries(httpserver PRIVATE ws2_32 psapi)
endif()