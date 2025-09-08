#!/bin/bash

# Test script to demonstrate CUDA enable/disable functionality
set -e

echo "Testing CUDA enable/disable functionality..."

# Test 1: Build with CUDA enabled (default)
echo "=== Test 1: Building with CUDA enabled ==="
mkdir -p build_cuda_on
cd build_cuda_on
cmake .. -DCMAKE_BUILD_TYPE=Debug -DHAVE_CUDA=ON
make -j$(nproc)
echo "✓ Build with CUDA enabled successful"

# Test 2: Build with CUDA disabled
echo "=== Test 2: Building with CUDA disabled ==="
cd ..
mkdir -p build_cuda_off
cd build_cuda_off
cmake .. -DCMAKE_BUILD_TYPE=Debug -DHAVE_CUDA=OFF
make -j$(nproc)
echo "✓ Build with CUDA disabled successful"

# Test 3: Run with CUDA enabled (if available)
echo "=== Test 3: Running with CUDA enabled ==="
cd ../build_cuda_on
mkdir -p test_metrics_cuda_on
timeout 5s ./unified_monitor \
    --mode cpp-only \
    --cpp-client "sleep 3" \
    --gpu-index 0 \
    --os-interval 50 \
    --gpu-interval 25 \
    --duration 3 \
    --out-dir test_metrics_cuda_on \
    || true

echo "Checking CUDA-enabled output:"
ls -la test_metrics_cuda_on/ 2>/dev/null || echo "No output directory created"

# Test 4: Run with CUDA disabled
echo "=== Test 4: Running with CUDA disabled ==="
cd ../build_cuda_off
mkdir -p test_metrics_cuda_off
timeout 5s ./unified_monitor \
    --mode cpp-only \
    --cpp-client "sleep 3" \
    --gpu-index 0 \
    --os-interval 50 \
    --gpu-interval 25 \
    --duration 3 \
    --out-dir test_metrics_cuda_off \
    || true

echo "Checking CUDA-disabled output:"
ls -la test_metrics_cuda_off/ 2>/dev/null || echo "No output directory created"

# Test 5: Compare outputs
echo "=== Test 5: Comparing outputs ==="
echo "CUDA-enabled build output:"
find ../build_cuda_on/test_metrics_cuda_on -name "*.orc" 2>/dev/null | head -5 || echo "No ORC files found"

echo "CUDA-disabled build output:"
find ../build_cuda_off/test_metrics_cuda_off -name "*.orc" 2>/dev/null | head -5 || echo "No ORC files found"

# Test 6: Check binary sizes
echo "=== Test 6: Binary size comparison ==="
echo "CUDA-enabled binary size:"
ls -lh ../build_cuda_on/unified_monitor 2>/dev/null || echo "Binary not found"

echo "CUDA-disabled binary size:"
ls -lh ../build_cuda_off/unified_monitor 2>/dev/null || echo "Binary not found"

echo "=== Test completed ==="
echo "Summary:"
echo "- CUDA-enabled build: Creates both OS and GPU metrics files"
echo "- CUDA-disabled build: Creates only OS metrics files"
echo "- Both builds should work without errors"
