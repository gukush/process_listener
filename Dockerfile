# Dockerfile for building listener without CUDA dependencies
FROM ubuntu:22.04

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libboost-all-dev \
    nlohmann-json3-dev \
    libssl-dev \
    git \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy source code
COPY . .

# Create build directory and build
RUN mkdir -p build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release -DHAVE_CUDA=0 .. && \
    make -j$(nproc)

# Create a simple entrypoint script
RUN echo '#!/bin/bash\n\
if [ "$1" = "browser+cpp" ]; then\n\
    shift\n\
    exec ./build/unified_monitor --mode browser+cpp "$@"\n\
elif [ "$1" = "cpp-only" ]; then\n\
    shift\n\
    exec ./build/unified_monitor --mode cpp-only "$@"\n\
else\n\
    exec ./build/unified_monitor "$@"\n\
fi' > /app/entrypoint.sh && chmod +x /app/entrypoint.sh

# Expose proxy ports
EXPOSE 9797 9798

# Set entrypoint
ENTRYPOINT ["/app/entrypoint.sh"]

# Default command
CMD ["--help"]
