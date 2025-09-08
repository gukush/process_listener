#!/bin/bash

# Test script for simplified listener
set -e

echo "Testing simplified listener with Apache ORC storage..."

# Build the project
echo "Building..."
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

# Create test output directory
mkdir -p test_metrics

# Test 1: C++ client only mode (5 seconds)
echo "Test 1: C++ client only mode (5 seconds)"
timeout 10s ./unified_monitor \
    --mode cpp-only \
    --cpp-client "sleep 8" \
    --gpu-index 0 \
    --os-interval 50 \
    --gpu-interval 25 \
    --duration 5 \
    --out-dir test_metrics \
    || true

# Check if files were created
echo "Checking output files..."
ls -la test_metrics/

if [ -f test_metrics/os_metrics_*.orc ]; then
    echo "✓ OS metrics files created"
    # Show file info
    file test_metrics/os_metrics_*.orc
else
    echo "✗ No OS metrics files found"
fi

if [ -f test_metrics/gpu_metrics_*.orc ]; then
    echo "✓ GPU metrics files created"
    # Show file info
    file test_metrics/gpu_metrics_*.orc
else
    echo "✗ No GPU metrics files found (NVML may not be available)"
fi

# Test 2: Configuration file mode
echo "Test 2: Configuration file mode"
cat > test_config.json << EOF
{
  "mode": "cpp-only",
  "cpp_client": {
    "enabled": true,
    "argv": ["sleep", "3"],
    "shell": false
  },
  "gpu_index": 0,
  "os_interval_ms": 50,
  "gpu_interval_ms": 25,
  "duration_sec": 3,
  "output_dir": "test_metrics_config",
  "storage": {
    "max_rows_per_file": 1000,
    "max_file_age_minutes": 1,
    "use_zstd_compression": true,
    "zstd_compression_level": 1
  }
}
EOF

mkdir -p test_metrics_config
timeout 8s ./unified_monitor --config test_config.json || true

echo "Checking config-based output..."
ls -la test_metrics_config/

# Test 3: Verify ORC files can be read
echo "Test 3: Verifying ORC files can be read"

# Check if we have Python and pyarrow
if command -v python3 &> /dev/null && python3 -c "import pyarrow.orc" 2>/dev/null; then
    echo "Checking ORC files with Python..."
    python3 -c "
import pyarrow as pa
import pyarrow.orc as orc
import glob
import os

# Check OS metrics
os_files = glob.glob('test_metrics/os_metrics_*.orc')
if os_files:
    print(f'Found {len(os_files)} OS metrics files')
    df = orc.read_table(os_files[0]).to_pandas()
    print(f'OS metrics shape: {df.shape}')
    print(f'OS metrics columns: {list(df.columns)}')
    print(f'OS metrics sample:')
    print(df.head())
else:
    print('No OS metrics files found')

# Check GPU metrics
gpu_files = glob.glob('test_metrics/gpu_metrics_*.orc')
if gpu_files:
    print(f'Found {len(gpu_files)} GPU metrics files')
    df = orc.read_table(gpu_files[0]).to_pandas()
    print(f'GPU metrics shape: {df.shape}')
    print(f'GPU metrics columns: {list(df.columns)}')
    print(f'GPU metrics sample:')
    print(df.head())
else:
    print('No GPU metrics files found')
"
else
    echo "Python/pyarrow not available, skipping ORC verification"
fi

# Test 4: Check file sizes and compression
echo "Test 4: Checking file sizes and compression"
if [ -f test_metrics/os_metrics_*.orc ]; then
    echo "OS metrics file sizes:"
    ls -lh test_metrics/os_metrics_*.orc
fi

if [ -f test_metrics/gpu_metrics_*.orc ]; then
    echo "GPU metrics file sizes:"
    ls -lh test_metrics/gpu_metrics_*.orc
fi

echo "Test completed!"
echo "Output directory contents:"
find test_metrics* -name "*.orc" -exec ls -lh {} \;
