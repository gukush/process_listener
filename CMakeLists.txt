cmake_minimum_required(VERSION 3.15)
project(unified_monitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --------- Toggle CUDA/NVML features ---------
option(HAVE_CUDA "Enable CUDA/NVML-based GPU metrics collection" ON)

# Dependencies
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(nlohmann_json 3.2.0 REQUIRED)

# Sources common to all builds
set(SRCS
    main.cpp
    websocket_proxy.cpp
    csv_export.cpp
    aggregation_export.cpp
    os_metrics_linux.cpp
)

# NOTE: The old gpu_listener.cpp depends on NVML; we don't need it because
#       GPUMetricsCollector is implemented in main.cpp. If you want to use
#       that file instead, add it conditionally inside the HAVE_CUDA block.

add_executable(unified_monitor ${SRCS})

target_include_directories(unified_monitor
    PRIVATE
        ${Boost_INCLUDE_DIRS}
)

target_link_libraries(unified_monitor
    PRIVATE
        Boost::system
        Boost::thread
        nlohmann_json::nlohmann_json
        pthread
)

if(HAVE_CUDA)
    # NVML is typically provided by the NVIDIA driver as libnvidia-ml.so
    # On many distros, the dev package is 'libnvidia-ml-dev'.
    find_library(NVML_LIB nvidia-ml)
    if(NOT NVML_LIB)
        message(FATAL_ERROR "HAVE_CUDA is ON but NVML library (nvidia-ml) was not found.")
    endif()
    target_link_libraries(unified_monitor PRIVATE ${NVML_LIB})
    target_compile_definitions(unified_monitor PRIVATE HAVE_CUDA=1)
else()
    target_compile_definitions(unified_monitor PRIVATE HAVE_CUDA=0)
endif()

# Build:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHAVE_CUDA=0
#   cmake --build build -j
#
# Run:
#   ./build/unified_monitor --mode browser+cpp --server ws://127.0.0.1:8765 \
#       --browser-path /usr/bin/google-chrome --browser-url http://localhost:3000 \
#       --cpp-client ./cpp_client --out-dir ./metrics
