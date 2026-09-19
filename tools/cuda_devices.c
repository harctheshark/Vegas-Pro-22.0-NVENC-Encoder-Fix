// cuda_devices - enumerates CUDA devices exactly as mxavcaacplug.dll does.
//
// The plugin selects its encode GPU by CUDA ordinal and refuses to continue if
// the ordinal is out of range. Its string table carries the two failure
// messages from the classic NVIDIA sample code:
//
//     Invalid Device Id = %d
//     GPU %d does not have NVENC capabilities exiting
//
// The first fires when deviceID > cuDeviceGetCount()-1. That matters on a
// machine with a non-NVIDIA GPU present, because Windows' adapter list and
// CUDA's device list are NOT the same list: CUDA only ever enumerates NVIDIA
// GPUs. If the host picks an index from the Windows list and hands it to CUDA,
// the two disagree and the encoder bails before making a single NVENC call.
//
// Loads nvcuda.dll dynamically, so no CUDA toolkit is needed to build this.

#include <windows.h>
#include <stdio.h>

typedef int CUdevice;
typedef int CUresult;

typedef CUresult(__cdecl *PFN_cuInit)(unsigned int);
typedef CUresult(__cdecl *PFN_cuDeviceGetCount)(int *);
typedef CUresult(__cdecl *PFN_cuDeviceGet)(CUdevice *, int);
typedef CUresult(__cdecl *PFN_cuDeviceGetName)(char *, int, CUdevice);
typedef CUresult(__cdecl *PFN_cuDeviceComputeCapability)(int *, int *, CUdevice);

int main(void)
{
    HMODULE lib = LoadLibraryA("nvcuda.dll");
    if (!lib) { printf("FATAL: nvcuda.dll not loadable (err %lu)\n", GetLastError()); return 2; }

    PFN_cuInit                    cuInit    = (PFN_cuInit)GetProcAddress(lib, "cuInit");
    PFN_cuDeviceGetCount          cuCount   = (PFN_cuDeviceGetCount)GetProcAddress(lib, "cuDeviceGetCount");
    PFN_cuDeviceGet               cuGet     = (PFN_cuDeviceGet)GetProcAddress(lib, "cuDeviceGet");
    PFN_cuDeviceGetName           cuName    = (PFN_cuDeviceGetName)GetProcAddress(lib, "cuDeviceGetName");
    PFN_cuDeviceComputeCapability cuCompCap = (PFN_cuDeviceComputeCapability)GetProcAddress(lib, "cuDeviceComputeCapability");

    printf("=== cuda_devices ===\n");
    printf("cuInit                    : %s\n", cuInit    ? "present" : "MISSING");
    printf("cuDeviceGetCount          : %s\n", cuCount   ? "present" : "MISSING");
    printf("cuDeviceComputeCapability : %s\n", cuCompCap ? "present" : "MISSING");
    if (!cuInit || !cuCount || !cuGet || !cuCompCap) return 2;

    CUresult r = cuInit(0);
    printf("\ncuInit(0)                 -> %d %s\n", r, r ? "FAILED" : "ok");
    if (r) return 1;

    int count = 0;
    r = cuCount(&count);
    printf("cuDeviceGetCount          -> %d, count = %d\n", r, count);
    printf("  => valid CUDA device ids are 0 .. %d\n\n", count - 1);

    for (int i = 0; i < count; i++) {
        CUdevice dev = 0;
        if (cuGet(&dev, i) != 0) { printf("  [%d] cuDeviceGet FAILED\n", i); continue; }
        char name[256] = "?";
        if (cuName) cuName(name, sizeof(name), dev);
        int major = 0, minor = 0;
        CUresult cc = cuCompCap(&major, &minor, dev);
        printf("  CUDA device %d : %s\n", i, name);
        printf("                  compute capability %d.%d (cuDeviceComputeCapability -> %d)\n",
               major, minor, cc);
        // The sample code this plugin is derived from rejects anything below 3.0.
        printf("                  NVENC-capable by the plugin's own test (>= 3.0): %s\n",
               ((major << 4) + minor) >= 0x30 ? "yes" : "NO");
    }

    printf("\nAny device id the host passes above %d produces\n", count - 1);
    printf("\"Invalid Device Id\" and the encoder gives up before calling NVENC.\n");
    return 0;
}
