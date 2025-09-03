cmake_minimum_required(VERSION 3.15)
project(gpu_listener LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS system thread)
find_package(nlohmann_json 3.2.0 REQUIRED)

add_executable(gpu_listener
    gpu_listener.cpp
)

target_include_directories(gpu_listener PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(gpu_listener
    PRIVATE
        Boost::system
        Boost::thread
        nlohmann_json::nlohmann_json
        nvidia-ml
        pthread
)

# If nvml is not on the default linker path, hint it:
# target_link_directories(gpu_listener PRIVATE /usr/lib/x86_64-linux-gnu)

#cd native_client/listener
#cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#cmake --build build -j
#./build/gpu_listener 8765 127.0.0.1