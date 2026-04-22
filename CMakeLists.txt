cmake_minimum_required(VERSION 3.21)
project(voicecontrol)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(geode)

file(GLOB_RECURSE SOURCES src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE src/libs)

target_link_libraries(${PROJECT_NAME}
    geode-sdk
    aaudio
    OpenSLES
    log
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    MA_ENABLE_AAUDIO
    MA_ENABLE_OPENSL
)

setup_geode_mod(${PROJECT_NAME})
