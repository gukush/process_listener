# Simplified Listener/Orchestrator

This is a simplified version of the listener that focuses on process spawning and efficient metrics collection using Apache ORC with Zstd compression.

## Key Changes from Original

- **Removed WebSocket handling**: No more chunk tracking or message interception
- **Removed `?listener` argument**: Browser clients no longer need special listener mode
- **Simplified metrics collection**: Just collects OS and GPU metrics continuously
- **Efficient storage**: Uses Apache ORC with Zstd compression for optimal storage
- **File rolling**: Automatically rolls files by time (5 min) or size (100k rows)

## Features

- Spawns browser and/or native clients based on configuration
- Collects OS metrics (CPU, memory, disk, network) every 100ms
- Collects GPU metrics (power, utilization, memory) every 50ms via NVML
- Stores metrics efficiently in ORC format with Zstd compression
- Automatic file rolling to keep files manageable
- JSON configuration support with CLI overrides

## Dependencies

- Apache ORC C++
- Apache ORC C++
- nlohmann/json
- NVML (for GPU metrics, optional)

## Building

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install liborc-dev libnlohmann-json-dev

# For GPU support (optional)
sudo apt-get install nvidia-ml-dev  # or install CUDA toolkit

# Or use vcpkg
vcpkg install orc nlohmann-json

# Build with GPU support (default)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DHAVE_CUDA=ON
make -j$(nproc)

# Build without GPU support (OS metrics only)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DHAVE_CUDA=OFF
make -j$(nproc)
```

## Usage

### With Configuration File

```bash
./unified_monitor --config config-simple.json
```

### With CLI Arguments

```bash
# Browser + C++ client
./unified_monitor --mode browser+cpp \
  --browser-path /usr/bin/google-chrome \
  --browser-url http://localhost:3000 \
  --cpp-client ./cpp_client \
  --gpu-index 0 \
  --os-interval 100 \
  --gpu-interval 50 \
  --out-dir ./metrics

# C++ client only
./unified_monitor --mode cpp-only \
  --cpp-client ./cpp_client \
  --gpu-index 0 \
  --out-dir ./metrics
```

## Configuration

The configuration file supports:

```json
{
  "mode": "browser+cpp",  // or "cpp-only"
  "server": "ws://127.0.0.1:8765",

  "browser": {
    "enabled": true,
    "argv": ["/usr/bin/google-chrome", "--new-window", "http://localhost:3000"],
    "cwd": "/",
    "env": { "DISPLAY": ":0" },
    "shell": false
  },

  "cpp_client": {
    "enabled": true,
    "argv": ["./cpp_client"],
    "cwd": "/home/user/project",
    "env": { },
    "shell": false
  },

  "gpu_index": 0,
  "os_interval_ms": 100,    // OS metrics every 100ms
  "gpu_interval_ms": 50,    // GPU metrics every 50ms
  "duration_sec": 0,        // 0 = run until signal
  "output_dir": "./metrics",

  "storage": {
    "max_rows_per_file": 100000,      // Roll files every 100k rows
    "max_file_age_minutes": 5,        // Roll files every 5 minutes
    "use_zstd_compression": true,     // Use Zstd compression
    "zstd_compression_level": 3       // Compression level (1-22)
  }
}
```

## Output Format

Metrics are stored in ORC files with the following schemas:

### OS Metrics (`os_metrics_YYYYMMDD_HHMMSS_XXX.orc`)
- `timestamp`: double (seconds since epoch)
- `pid`: int32 (process ID)
- `cpu_percent`: float32 (CPU usage percentage)
- `mem_rss_kb`: int64 (resident memory in KB)
- `mem_vms_kb`: int64 (virtual memory in KB)
- `disk_read_bytes`: uint64 (disk read bytes)
- `disk_write_bytes`: uint64 (disk write bytes)
- `net_recv_bytes`: uint64 (network received bytes)
- `net_sent_bytes`: uint64 (network sent bytes)

### GPU Metrics (`gpu_metrics_YYYYMMDD_HHMMSS_XXX.orc`)
- `timestamp`: double (seconds since epoch)
- `gpu_index`: uint32 (GPU index)
- `power_mw`: uint32 (power usage in milliwatts)
- `gpu_util_percent`: int32 (GPU utilization percentage)
- `mem_util_percent`: int32 (memory utilization percentage)
- `mem_used_bytes`: uint64 (used memory in bytes)
- `sm_clock_mhz`: uint32 (SM clock in MHz)

## Converting to CSV

To convert ORC files to CSV for analysis:

```bash
# Using DuckDB CLI
duckdb -c "COPY (SELECT * FROM read_orc('os_metrics_*.orc')) TO 'os_metrics.csv' WITH (HEADER, DELIMITER ',');"
duckdb -c "COPY (SELECT * FROM read_orc('gpu_metrics_*.orc')) TO 'gpu_metrics.csv' WITH (HEADER, DELIMITER ',');"

# Using Python with pyarrow
import pyarrow.orc as orc
import pandas as pd
import glob

# Read all OS metrics files
os_files = glob.glob('os_metrics_*.orc')
os_tables = [orc.read_table(f) for f in os_files]
os_df = pa.concat_tables(os_tables).to_pandas()
os_df.to_csv('os_metrics.csv', index=False)

# Read all GPU metrics files
gpu_files = glob.glob('gpu_metrics_*.orc')
gpu_tables = [orc.read_table(f) for f in gpu_files]
gpu_df = pa.concat_tables(gpu_tables).to_pandas()
gpu_df.to_csv('gpu_metrics.csv', index=False)
```

## Performance Notes

- **Zstd compression**: Reduces file sizes by 60-80% with minimal CPU overhead
- **Columnar storage**: ORC's columnar format enables efficient analytics
- **File rolling**: Prevents individual files from becoming too large
- **Batch writing**: Metrics are written in batches for better performance
- **Memory efficient**: Uses streaming writes, doesn't keep all data in memory

## Troubleshooting

1. **NVML not found**: Build with `-DHAVE_CUDA=OFF` to disable GPU metrics
2. **ORC not found**: Install via package manager or vcpkg
3. **Permission issues**: Ensure write access to output directory
4. **Process spawn failures**: Check paths and environment variables in config
5. **CUDA compilation errors**: Use `-DHAVE_CUDA=OFF` if you don't need GPU metrics
