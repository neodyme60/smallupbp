cmake_minimum_required(VERSION 2.8)
project (smallUPBP)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules)

find_package(OpenEXR REQUIRED)
find_package(IlmBase REQUIRED)

set(CMAKE_CXX_FLAGS "-std=gnu++11 -Wno-narrowing -msse4.1 -fpermissive")

add_subdirectory (embree-2.0)

include_directories("embree-2.0/embree/")
add_subdirectory (SmallUPBP)
