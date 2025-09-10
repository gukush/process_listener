cmake_minimum_required(VERSION 3.15)
project(unified_monitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --------- Toggle CUDA/NVML features ---------
option(HAVE_CUDA "Enable CUDA/NVML-based GPU metrics collection" ON)

# Detect vcpkg toolchain usage
set(USING_VCPKG OFF)
if(DEFINED CMAKE_TOOLCHAIN_FILE AND CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg")
    set(USING_VCPKG ON)
endif()

# Dependencies
# Boost: only 'system' is required (we use std::thread)
if(USING_VCPKG)
    find_package(Boost CONFIG REQUIRED COMPONENTS system)
else()
    find_package(Boost REQUIRED COMPONENTS system)
endif()

# nlohmann_json: prefer CONFIG (vcpkg), otherwise system package
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    find_package(nlohmann_json REQUIRED)
endif()

# OpenSSL: prefer CONFIG (vcpkg), otherwise system package
find_package(OpenSSL CONFIG QUIET)
if(NOT OpenSSL_FOUND)
    find_package(OpenSSL REQUIRED)
endif()

# Apache ORC: prefer CONFIG (vcpkg), otherwise system package
find_package(orc CONFIG QUIET)
if(NOT orc_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ORC REQUIRED orc)
endif()

# Sources (note: we DO NOT compile orchestrator.cpp; we use simple_orchestrator.hpp)
set(SRCS
    simple_main.cpp
    metrics_storage.cpp
    os_metrics_linux.cpp
)

add_executable(unified_monitor ${SRCS})

target_include_directories(unified_monitor
    PRIVATE
        ${Boost_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}  # for simple_orchestrator.hpp
)

# Add ORC include directories
if(orc_FOUND)
    if(TARGET orc::orc)
        # Modern CMake target
        target_link_libraries(unified_monitor PRIVATE orc::orc)
    else()
        # Fallback for pkg-config
        target_include_directories(unified_monitor PRIVATE ${ORC_INCLUDE_DIRS})
        target_link_libraries(unified_monitor PRIVATE ${ORC_LIBRARIES})
    endif()
else()
    # Fallback: try to find ORC manually
    find_library(ORC_LIB orc
        HINTS /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
        PATH_SUFFIXES lib)
    if(ORC_LIB)
        target_link_libraries(unified_monitor PRIVATE ${ORC_LIB})
    else()
        message(FATAL_ERROR "ORC library not found. Install liborc-dev or use vcpkg.")
    endif()
endif()

target_link_libraries(unified_monitor
    PRIVATE
        Boost::system
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto
        pthread
)

# NVML (CUDA) wiring
if(HAVE_CUDA)
    if(NOT NVML_INCLUDE_DIR)
        find_path(NVML_INCLUDE_DIR nvml.h
                  HINTS /usr/include /usr/include/nvidia-ml /usr/local/cuda/include
                        /usr/local/cuda-13.0/targets/x86_64-linux/include
                  PATH_SUFFIXES nvidia-ml)
    endif()
    if(NOT NVML_INCLUDE_DIR)
        message(FATAL_ERROR "HAVE_CUDA=ON but NVML include dir not found. "
                            "Set -DNVML_INCLUDE_DIR=/path/to where nvml.h exists, "
                            "or set -DHAVE_CUDA=0 to disable GPU metrics.")
    endif()
    target_include_directories(unified_monitor PRIVATE ${NVML_INCLUDE_DIR})

    if(NOT NVML_LIB)
        find_library(NVML_LIB nvidia-ml
                     HINTS /usr/lib /usr/lib/x86_64-linux-gnu
                           /usr/local/cuda/lib64
                           /usr/local/cuda-13.0/targets/x86_64-linux/lib
                           /usr/local/cuda-13.0/targets/x86_64-linux/lib/stubs)
    endif()
    if(NOT NVML_LIB)
        message(FATAL_ERROR "HAVE_CUDA=ON but NVML library not found. "
                            "Set -DNVML_LIB=/path/to/libnvidia-ml.so, "
                            "or set -DHAVE_CUDA=0 to disable GPU metrics.")
    endif()
    target_link_libraries(unified_monitor PRIVATE ${NVML_LIB})
    target_compile_definitions(unified_monitor PRIVATE HAVE_CUDA=1)
else()
    target_compile_definitions(unified_monitor PRIVATE HAVE_CUDA=0)
    # Use the existing nvml_stub.h file when CUDA is disabled
    target_include_directories(unified_monitor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Build:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build -j