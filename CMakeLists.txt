cmake_minimum_required(VERSION 3.21)

project(VisualTC
    VERSION 0.3.0
    DESCRIPTION "VisualTC — DICOM Medical Image Viewer"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(VISUALTC_BUILD_APP "Build the Qt desktop application" ON)
option(VISUALTC_BUILD_TESTS "Build unit tests" ON)
option(VISUALTC_ENABLE_SANITIZERS "Enable AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
option(VISUALTC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(VISUALTC_FETCH_CATCH2 "Download Catch2 if it is not installed" ON)

include(CompilerSettings)

find_package(Threads REQUIRED)
find_package(GDCM 3.0 REQUIRED CONFIG)
include(LibArchiveSupport)  # ZIP / 7z / RAR / TAR / GZ / ISO reading

add_subdirectory(src)

if(VISUALTC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(VISUALTC_BUILD_APP)
    include(Packaging)
endif()

message(STATUS "VisualTC ${PROJECT_VERSION}: app=${VISUALTC_BUILD_APP} tests=${VISUALTC_BUILD_TESTS} "
               "sanitizers=${VISUALTC_ENABLE_SANITIZERS}")
