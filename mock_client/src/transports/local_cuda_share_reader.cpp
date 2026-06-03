#include "transports/local_cuda_share_reader.h"

#include <cstdlib>
#include <cstring>
#include <sstream>

#include <cuda.h>

namespace notch_mock
{

namespace
{

bool CheckCuda(CUresult result, const std::string& operation, std::string& error)
{
    if (result == CUDA_SUCCESS)
    {
        return true;
    }
    const char* name = nullptr;
    const char* text = nullptr;
    cuGetErrorName(result, &name);
    cuGetErrorString(result, &text);
    error = operation + " failed";
    if (name != nullptr)
    {
        error += " ";
        error += name;
    }
    if (text != nullptr)
    {
        error += ": ";
        error += text;
    }
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
    if (!HexToBytes(share.m_ipcHandleHex, handleBytes) || handleBytes.size() != sizeof(CUipcMemHandle))
    {
        error = "cuda ipc_handle is not a 64-byte hex handle";
        return false;
    }

    CUresult initResult = cuInit(0);
    if (!CheckCuda(initResult, "cuInit", error))
    {
        return false;
    }

    CUdevice device;
    const int deviceIndex = share.m_cudaDeviceIndex >= 0 ? share.m_cudaDeviceIndex : 0;
    if (!CheckCuda(cuDeviceGet(&device, deviceIndex), "cuDeviceGet", error))
    {
        return false;
    }

    CUcontext context;
    if (!CheckCuda(cuDevicePrimaryCtxRetain(&context, device), "cuDevicePrimaryCtxRetain", error))
    {
        return false;
    }

    bool contextPushed = false;
    CUdeviceptr devicePtr = 0;
    bool ok = false;
    do
    {
        if (!CheckCuda(cuCtxPushCurrent(context), "cuCtxPushCurrent", error))
        {
            break;
        }
        contextPushed = true;

        CUipcMemHandle ipcHandle;
        std::memset(&ipcHandle, 0, sizeof(ipcHandle));
        std::memcpy(&ipcHandle, &handleBytes[0], sizeof(ipcHandle));
        if (!CheckCuda(cuIpcOpenMemHandle(&devicePtr, ipcHandle, CU_IPC_MEM_LAZY_ENABLE_PEER_ACCESS),
                       "cuIpcOpenMemHandle", error))
        {
            break;
        }

        bytes.resize(static_cast<size_t>(share.m_dataSizeBytes));
        if (!CheckCuda(cuMemcpyDtoH(&bytes[0], devicePtr + share.m_dataOffset, bytes.size()), "cuMemcpyDtoH", error))
        {
            break;
        }
        ok = true;
    } while (false);

    if (devicePtr != 0)
    {
        std::string closeError;
        CheckCuda(cuIpcCloseMemHandle(devicePtr), "cuIpcCloseMemHandle", closeError);
    }
    if (contextPushed)
    {
        CUcontext popped;
        std::string popError;
        CheckCuda(cuCtxPopCurrent(&popped), "cuCtxPopCurrent", popError);
    }
    std::string releaseError;
    CheckCuda(cuDevicePrimaryCtxRelease(device), "cuDevicePrimaryCtxRelease", releaseError);

    if (!ok)
    {
        bytes.clear();
    }
    return ok;
}

} // namespace notch_mock
