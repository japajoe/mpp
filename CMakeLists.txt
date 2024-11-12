cmake_minimum_required(VERSION 3.12)

project(mpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS_RELEASE "-O3")
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

file(GLOB_RECURSE SOURCES src/*.cpp src/*.c)

include_directories(
    "${PROJECT_SOURCE_DIR}/include/"
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})
#add_executable(${PROJECT_NAME} ${SOURCES})

if(BUILD_PLATFORM STREQUAL "linux")
    target_link_libraries(${PROJECT_NAME} -pthread -ldl -lm)
elseif(BUILD_PLATFORM STREQUAL "windows_mingw")
    set(CMAKE_C_FLAGS "-static")
    set(CMAKE_CXX_FLAGS "-static")
    add_definitions(-DWINVER=0x0601)
    add_definitions(-D_WIN32_WINNT=0x0601)
    target_link_libraries(${PROJECT_NAME} -pthread -lwinmm -lws2_32)
endif()


