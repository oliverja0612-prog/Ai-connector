cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(AIConnector VERSION 1.0.0)

if (MSVC AND NOT CMAKE_CXX_FLAGS)
    set(CMAKE_CXX_FLAGS "/EHsc" CACHE STRING "MSVC C++ compiler flags" FORCE)
endif()

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Set GEODE_SDK to the installed Geode SDK directory.")
endif()

add_library(${PROJECT_NAME} SHARED
    src/ApiServer.cpp
    src/main.cpp
)
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})

if (WIN32)
    target_link_libraries(${PROJECT_NAME} ws2_32 user32 gdi32)
endif()
