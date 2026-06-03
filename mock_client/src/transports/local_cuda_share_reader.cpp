#include "transports/local_cuda_share_reader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <cuda.h>          // driver API (cuIpcOpenMemHandle)
#include <cuda_runtime.h>  // runtime API (cudaIpcOpenMemHandle)

// The server exports the output share with the *driver* API (cuMemAlloc +
// cuIpcGetMemHandle), and the real Notch consumer imports with the *runtime*
// API (cudaIpcOpenMemHandle / cudaIpcMemHandle_t — see NGXGetIpcMemHandle /
// NGXOpenIpcMemHandle). That driver-export / runtime-import pairing works on
// native Windows. To find out whether it also works on this client (e.g. WSL2),
// the reader tries the runtime open first (the production path) and, if that
// returns cudaErrorInvalidResourceHandle, falls back to the driver open
// (cuIpcOpenMemHandle) so a single run tells us which family the platform
// actually honors:
//   - runtime open succeeds  -> the production pairing works here
//   - driver open succeeds   -> the platform needs a matched driver-open
//   - both fail              -> the platform has no usable CUDA IPC at all
// The winning path is printed to stdout (captured in mock-client.log); when
// both fail, both driver reasons are returned in `error`.

namespace notch_mock
{

namespace
{

bool CheckCuda(cudaError_t result, const std::string& operation, std::string& error)
{
    if (result == cudaSuccess)
    {
        return true;
    }
    error = operation + " failed ";
    error += cudaGetErrorName(result);
    error += ": ";
    error += cudaGetErrorString(result);
    return false;
}

std::string DriverError(CUresult result, const std::string& operation)
{
    const char* name = nullptr;
    const char* desc = nullptr;
    cuGetErrorName(result, &name);
    cuGetErrorString(result, &desc);
    std::string error = operation + " failed ";
    error += (name != nullptr) ? name : "CUDA_ERROR";
    error += ": ";
    error += (desc != nullptr) ? desc : "unknown driver error";
    return error;
}

// Save the current runtime device on construction and restore it on destruction,
// so a case that binds a specific device for an IPC import never leaks that
// binding into the next case (which may target a different device index).
struct DeviceGuard
{
    int previous = 0;
    DeviceGuard() { cudaGetDevice(&previous); }
    ~DeviceGuard() { cudaSetDevice(previous); }
};

// Same idea for the driver-API current context: the driver fallback retains and
// sets the device primary context, so restore whatever context was current
// before, otherwise a released/foreign context leaks to subsequent work.
struct ContextGuard
{
    CUcontext previous = nullptr;
    ContextGuard() { cuCtxGetCurrent(&previous); }
    ~ContextGuard() { cuCtxSetCurrent(previous); }
};

bool HexToBytes(const std::string& hex, std::vector<uint8_t>& bytes)
{
    bytes.clear();
    if ((hex.size() % 2U) != 0U)
    {
        return false;
    }
    for (size_t i = 0; i < hex.size(); i += 2U)
    {
        const std::string byteText = hex.substr(i, 2U);
        char* end = nullptr;
        const unsigned long value = std::strtoul(byteText.c_str(), &end, 16);
        if (end == nullptr || *end != '\0' || value > 255UL)
        {
            return false;
        }
        bytes.push_back(static_cast<uint8_t>(value));
    }
    return true;
}

// Open the share via the runtime API (cudaIpcOpenMemHandle), copy the data
// region to host, and close. Returns true on success; on failure `error`
// carries the runtime reason.
bool TryRuntimeOpen(
    const std::vector<uint8_t>& handleBytes,
    long dataOffset,
    std::vector<uint8_t>& bytes,
    std::string& error)
{
    cudaIpcMemHandle_t handle;
    std::memset(&handle, 0, sizeof(handle));
    std::memcpy(&handle, &handleBytes[0], sizeof(handle));

    void* devicePtr = nullptr;
    if (!CheckCuda(cudaIpcOpenMemHandle(&devicePtr, handle, cudaIpcMemLazyEnablePeerAccess),
                   "cudaIpcOpenMemHandle", error))
    {
        return false;
    }

    const cudaError_t copyResult = cudaMemcpy(
        &bytes[0],
        static_cast<const char*>(devicePtr) + dataOffset,
        bytes.size(),
        cudaMemcpyDeviceToHost);

    std::string closeError;
    CheckCuda(cudaIpcCloseMemHandle(devicePtr), "cudaIpcCloseMemHandle", closeError);

    return CheckCuda(copyResult, "cudaMemcpy", error);
}

// Open the share via the driver API (cuIpcOpenMemHandle) on the device's
// primary context, copy the data region to host, and close. Returns true on
// success; on failure `error` carries the driver reason.
bool TryDriverOpen(
    int deviceIndex,
    const std::vector<uint8_t>& handleBytes,
    long dataOffset,
    std::vector<uint8_t>& bytes,
    std::string& error)
{
    // Restore the previously-current driver context on every exit.
    ContextGuard contextGuard;

    CUresult initResult = cuInit(0);
    if (initResult != CUDA_SUCCESS)
    {
        error = DriverError(initResult, "cuInit");
        return false;
    }

    CUdevice device = 0;
    CUresult devResult = cuDeviceGet(&device, deviceIndex);
    if (devResult != CUDA_SUCCESS)
    {
        error = DriverError(devResult, "cuDeviceGet");
        return false;
    }

    // Use the primary context so the mapping shares the runtime's context
    // lineage on this device (the runtime API also uses the primary context).
    CUcontext context = nullptr;
    CUresult ctxResult = cuDevicePrimaryCtxRetain(&context, device);
    if (ctxResult != CUDA_SUCCESS)
    {
        error = DriverError(ctxResult, "cuDevicePrimaryCtxRetain");
        return false;
    }
    cuCtxSetCurrent(context);

    CUipcMemHandle handle;
    std::memset(&handle, 0, sizeof(handle));
    std::memcpy(&handle, &handleBytes[0], sizeof(handle));

    CUdeviceptr devicePtr = 0;
    CUresult openResult = cuIpcOpenMemHandle(&devicePtr, handle, CU_IPC_MEM_LAZY_ENABLE_PEER_ACCESS);
    if (openResult != CUDA_SUCCESS)
    {
        error = DriverError(openResult, "cuIpcOpenMemHandle");
        cuDevicePrimaryCtxRelease(device);
        return false;
    }

    CUresult copyResult =
        cuMemcpyDtoH(&bytes[0], devicePtr + static_cast<size_t>(dataOffset), bytes.size());

    cuIpcCloseMemHandle(devicePtr);
    cuDevicePrimaryCtxRelease(device);

    if (copyResult != CUDA_SUCCESS)
    {
        error = DriverError(copyResult, "cuMemcpyDtoH");
        return false;
    }
    return true;
}

} // namespace

bool LocalCudaShareReader::CudaAvailable(std::string& error)
{
    int deviceCount = 0;
    if (!CheckCuda(cudaGetDeviceCount(&deviceCount), "cudaGetDeviceCount", error))
    {
        return false;
    }
    if (deviceCount <= 0)
    {
        error = "no CUDA device present";
        return false;
    }
    return true;
}

bool LocalCudaShareReader::ReadShare(
    const notch_comfy::CudaShareStatus& share,
    std::vector<uint8_t>& bytes,
    std::string& error)
{
    bytes.clear();
    if (share.m_ipcHandleHex.empty())
    {
        error = "cuda share status did not include ipc_handle";
        return false;
    }
    if (share.m_dataSizeBytes <= 0)
    {
        error = "cuda share status did not include data_size_bytes";
        return false;
    }

    std::vector<uint8_t> handleBytes;
    if (!HexToBytes(share.m_ipcHandleHex, handleBytes) || handleBytes.size() != sizeof(cudaIpcMemHandle_t))
    {
        error = "cuda ipc_handle is not a 64-byte hex handle";
        return false;
    }

    const int deviceIndex = share.m_cudaDeviceIndex >= 0 ? share.m_cudaDeviceIndex : 0;
    // Restore the previous current device/context on every exit so binding the
    // share's device for this import cannot leak into the next case.
    DeviceGuard deviceGuard;
    if (!CheckCuda(cudaSetDevice(deviceIndex), "cudaSetDevice", error))
    {
        return false;
    }
    // The import must run on exactly the device the server published. Confirm the
    // binding took effect so a stale/leaked current device can't make us read the
    // wrong GPU's memory and report a false match.
    int boundDevice = -1;
    if (!CheckCuda(cudaGetDevice(&boundDevice), "cudaGetDevice", error))
    {
        return false;
    }
    if (boundDevice != deviceIndex)
    {
        error = "cuda device binding did not take effect (wanted " + std::to_string(deviceIndex) +
                ", current " + std::to_string(boundDevice) + ")";
        return false;
    }

    bytes.resize(static_cast<size_t>(share.m_dataSizeBytes));

    // 1. Production path first: runtime open (what real Notch does).
    std::string runtimeError;
    if (TryRuntimeOpen(handleBytes, share.m_dataOffset, bytes, runtimeError))
    {
        std::printf("cuda reader: opened share '%s' via runtime api (cudaIpcOpenMemHandle)\n",
                    share.m_name.c_str());
        std::fflush(stdout);
        return true;
    }

    // 2. Fallback: driver open, matching the server's cuIpcGetMemHandle export.
    std::string driverError;
    if (TryDriverOpen(deviceIndex, handleBytes, share.m_dataOffset, bytes, driverError))
    {
        std::printf("cuda reader: opened share '%s' via driver api (cuIpcOpenMemHandle) "
                    "after runtime open failed (%s)\n",
                    share.m_name.c_str(), runtimeError.c_str());
        std::fflush(stdout);
        return true;
    }

    bytes.clear();
    error = "runtime open: " + runtimeError + "; driver open: " + driverError;
    return false;
}

} // namespace notch_mock
