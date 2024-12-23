cmake_minimum_required(VERSION 3.8)
project(raddebugger)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Change path from /src if needed, or add more directories
file(GLOB_RECURSE raddbg_sources
        "${CMAKE_SOURCE_DIR}/src/*.c"
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
        )

list(FILTER raddbg_sources EXCLUDE REGEX ".+metagen_os_.+")
list(FILTER raddbg_sources EXCLUDE REGEX ".+metagen_base_.+")

add_executable(raddbg ${raddbg_sources})

target_include_directories(raddbg PUBLIC "${CMAKE_SOURCE_DIR}/src")
