cmake_minimum_required(VERSION 3.20)
project(Lensyum LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Path to the unpacked After Effects SDK (the folder that contains Examples/Headers).
set(AE_SDK_DIR "" CACHE PATH "After Effects SDK root")
option(LENSYUM_BUILD_TOOLS "Build the command-line test harness" ON)

find_package(Threads REQUIRED)

add_library(lensyum_core STATIC
    core/src/Lens.cpp
    core/src/Aperture.cpp
    core/src/PsfAtlas.cpp
    core/src/Renderer.cpp
)
target_include_directories(lensyum_core PUBLIC core/include)
target_link_libraries(lensyum_core PUBLIC Threads::Threads)
set_target_properties(lensyum_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(lensyum_core PRIVATE /O2 /fp:fast /W3)
else()
    target_compile_options(lensyum_core PRIVATE -O3 -Wall -Wextra)
endif()

if(LENSYUM_BUILD_TOOLS)
    add_executable(lensyum_cli tools/lensyum_cli.cpp)
    target_link_libraries(lensyum_cli PRIVATE lensyum_core)
endif()

if(AE_SDK_DIR)
    add_subdirectory(ae)
endif()
