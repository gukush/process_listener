#ifndef NVML_STUB_H
#define NVML_STUB_H

// Minimal NVML stub for when CUDA is disabled
// This file provides the necessary type definitions and function declarations
// for compilation when HAVE_CUDA=0

typedef int nvmlReturn_t;
typedef void* nvmlDevice_t;
typedef struct { unsigned int gpu; unsigned int memory; } nvmlUtilization_t;
typedef struct { unsigned long long used; } nvmlMemory_t;
typedef struct { unsigned int pid; unsigned int smUtil; } nvmlProcessUtilizationSample_t;

#define NVML_SUCCESS 0
#define NVML_CLOCK_SM 0

// Stub functions (no-ops when CUDA is disabled)
inline nvmlReturn_t nvmlInit_v2() { return NVML_SUCCESS; }
inline nvmlReturn_t nvmlShutdown() { return NVML_SUCCESS; }
inline nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int, nvmlDevice_t*) { return NVML_SUCCESS; }
inline nvmlReturn_t nvmlDeviceGetPowerUsage(nvmlDevice_t, unsigned int*) { return NVML_SUCCESS; }
inline nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t, nvmlUtilization_t*) { return NVML_SUCCESS; }
inline nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t, nvmlMemory_t*) { return NVML_SUCCESS; }
inline nvmlReturn_t nvmlDeviceGetClockInfo(nvmlDevice_t, int, unsigned int*) { return NVML_SUCCESS; }
inline nvmlReturn_t nvmlDeviceGetProcessUtilization(nvmlDevice_t, nvmlProcessUtilizationSample_t*, unsigned int*, unsigned long long) { return NVML_SUCCESS; }
inline const char* nvmlErrorString(nvmlReturn_t) { return "NVML disabled"; }

#endif // NVML_STUB_H

