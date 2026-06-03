#include "transports/local_cuda_share_reader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <cuda_runtime.h>  // runtime API (cudaIpcOpenMemHandle)

// Imports the output share with the CUDA *runtime* API (cudaIpcOpenMemHandle /
// cudaIpcMemHandle_t), exactly as the real Notch consumer does (NGXGetIpcMemHandle
// / NGXOpenIpcMemHandle). Runtime-only on purpose: linking the driver library
// (libcuda) would make this binary fail to load on a GPU-less container (the
// remote client has no driver), and cudart lazy-loads libcuda only when a CUDA
// call is actually made — so the binary loads everywhere and only touches CUDA
// where a device is present. The earlier driver-API fallback was a WSL2 dual-open
// experiment; that platform rejects the handle from both APIs (handled by the
// cuda_ipc_unsupported skip gate), so the fallback is gone.

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

// Save the current runtime device on construction and restore it on destruction,
// so a case that binds a specific device for an IPC import never leaks that
// binding into the next case (which may target a different device index).
struct DeviceGuard
{
    int previous = 0;
    DeviceGuard() { cudaGetDevice(&previous); }
    ~DeviceGuard() { cudaSetDevice(previous); }
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
    // Restore the previous current device on every exit so binding the share's
    // device for this import cannot leak into the next case.
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

    cudaIpcMemHandle_t handle;
    std::memset(&handle, 0, sizeof(handle));
    std::memcpy(&handle, &handleBytes[0], sizeof(handle));

    void* devicePtr = nullptr;
    if (!CheckCuda(cudaIpcOpenMemHandle(&devicePtr, handle, cudaIpcMemLazyEnablePeerAccess),
                   "cudaIpcOpenMemHandle", error))
    {
        return false;
    }

    bytes.resize(static_cast<size_t>(share.m_dataSizeBytes));
    const cudaError_t copyResult = cudaMemcpy(
        &bytes[0],
        static_cast<const char*>(devicePtr) + share.m_dataOffset,
        bytes.size(),
        cudaMemcpyDeviceToHost);

    std::string closeError;
    CheckCuda(cudaIpcCloseMemHandle(devicePtr), "cudaIpcCloseMemHandle", closeError);

    if (!CheckCuda(copyResult, "cudaMemcpy", error))
    {
        bytes.clear();
        return false;
    }
    std::printf("cuda reader: opened share '%s' via runtime api (cudaIpcOpenMemHandle)\n", share.m_name.c_str());
    std::fflush(stdout);
    return true;
}

} // namespace notch_mock
