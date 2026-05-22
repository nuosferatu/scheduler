cmake_minimum_required(VERSION 3.14)

project(scheduler
    VERSION   0.1.0
    LANGUAGES CXX
)

find_package(Threads REQUIRED)

add_library(scheduler INTERFACE)

target_include_directories(scheduler INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(scheduler INTERFACE
    Threads::Threads
)

target_compile_features(scheduler INTERFACE cxx_std_17)
