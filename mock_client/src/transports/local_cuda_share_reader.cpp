#include "transports/local_cuda_share_reader.h"

#include <cstdlib>
#include <cstring>

#include <cuda_runtime.h>

// Use the CUDA *runtime* API (cudaIpc*) to match the real Notch client, which
// imports output shares with cudaIpcOpenMemHandle / cudaIpcMemHandle_t (see
// Notch's NGXGetIpcMemHandle using cudaIpcGetMemHandle). The earlier driver-API
// (cuIpcOpenMemHandle) reader tested a different code path than production; the
// server exports a 64-byte handle that the runtime API opens, exactly as Notch
// does.

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
    if (!CheckCuda(cudaSetDevice(deviceIndex), "cudaSetDevice", error))
    {
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
    return true;
}

} // namespace notch_mock
