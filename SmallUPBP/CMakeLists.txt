cmake_minimum_required(VERSION 2.8)
file(GLOB smallUPBP_SRC
        "src/Beams/*.hxx"
        "src/Beams/*.cxx"
        "src/Bre/*.hxx"
        "src/Bre/*.cxx"
        "src/Misc/*.hxx"
        "src/Misc/*.cxx"
        "src/Path/*.hxx"
        "src/Path/*.cxx"
        "src/Renderers/*.hxx"
        "src/Renderers/*.cxx"
        "src/Scene/*.hxx"
        "src/Scene/*.cxx"
        "src/Structs/*.hxx"
        "src/Structs/*.cxx"
        "src/PrecompiledLibs/FastFloat/*.hxx"
        "src/PrecompiledLibs/FastFloat/*.cxx"
        "src/SmallUPBP.cxx"
        )

set(CMAKE_CXX_FLAGS "-std=gnu++11 -Wno-narrowing -msse4.1 -fpermissive -fopenmp")


include_directories("src/")
add_executable(smallUPBP ${smallUPBP_SRC})

target_link_libraries(smallUPBP embree ${ILMBASE_LIBRARIES} ${OPENEXR_LIBRARIES})