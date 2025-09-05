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
# Boost: only need 'system' (we don't use boost_thread)
if(USING_VCPKG)
    # vcpkg provides Boost in CONFIG mode
    find_package(Boost CONFIG REQUIRED COMPONENTS system)
else()
    # system Boost (e.g., 1.74 on Ubuntu)
    find_package(Boost REQUIRED COMPONENTS system)
endif()

# nlohmann_json: prefer CONFIG (vcpkg), otherwise normal find (system package)
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    find_package(nlohmann_json REQUIRED)
endif()

# Sources
set(SRCS
    main.cpp
    websocket_proxy.cpp
    csv_export.cpp
    aggregation_export.cpp
    os_metrics_linux.cpp
    # gpu_listener.cpp  # not needed; GPU collector is implemented in main.cpp
)

add_executable(unified_monitor ${SRCS})

target_include_directories(unified_monitor
    PRIVATE
        ${Boost_INCLUDE_DIRS}
)

target_link_libraries(unified_monitor
    PRIVATE
        Boost::system
        nlohmann_json::nlohmann_json
        pthread
)

# NVML (CUDA) wiring
if(HAVE_CUDA)
    # Allow user to specify exact library: -DNVML_LIB=/path/to/libnvidia-ml.so
    if(NOT NVML_LIB)
        find_library(NVML_LIB nvidia-ml)
    endif()
    if(NOT NVML_LIB)
        message(FATAL_ERROR "HAVE_CUDA is ON but NVML library not found. "
                            "Pass -DNVML_LIB=/path/to/libnvidia-ml.so or set -DHAVE_CUDA=0 to disable.")
    endif()
    target_link_libraries(unified_monitor PRIVATE ${NVML_LIB})
    target_compile_definitions(unified_monitor PRIVATE HAVE_CUDA=1)
else()
    target_compile_definitions(unified_monitor PRIVATE HAVE_CUDA=0)
endif()

# Build notes:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
