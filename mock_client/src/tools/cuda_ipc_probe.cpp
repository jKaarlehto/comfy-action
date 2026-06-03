#include <cuda.h>
#include <cuda_runtime.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

const size_t kAllocationBytes = 2U * 1024U * 1024U;

struct ProbePayload
{
    cudaIpcMemHandle_t memHandle;
    cudaIpcEventHandle_t eventHandle;
    size_t sizeBytes;
    unsigned long long expectedHash;
};

std::string JsonEscape(const std::string& value)
{
    std::ostringstream out;
    for (size_t i = 0; i < value.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        switch (ch)
        {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20U)
            {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(ch) << std::dec;
            }
            else
            {
                out << value[i];
            }
            break;
        }
    }
    return out.str();
}

std::string Quote(const std::string& value)
{
    return "\"" + JsonEscape(value) + "\"";
}

std::string Bool(bool value)
{
    return value ? "true" : "false";
}

std::string Hex64(unsigned long long value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

unsigned long long Fnv1a64(const std::vector<unsigned char>& bytes)
{
    unsigned long long hash = 1469598103934665603ULL;
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        hash ^= static_cast<unsigned long long>(bytes[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::vector<unsigned char> MakePattern(size_t sizeBytes)
{
    std::vector<unsigned char> bytes(sizeBytes);
    for (size_t i = 0; i < sizeBytes; ++i)
    {
        bytes[i] = static_cast<unsigned char>((i * 131U + 17U) & 0xffU);
    }
    return bytes;
}

std::string CudaResultJson(cudaError_t result, const std::string& operation)
{
    const char* name = cudaGetErrorName(result);
    const char* description = cudaGetErrorString(result);
    std::ostringstream out;
    out << "{\"operation\":" << Quote(operation)
        << ",\"ok\":" << Bool(result == cudaSuccess)
        << ",\"code\":" << static_cast<int>(result)
        << ",\"name\":" << Quote(name != nullptr ? name : "cudaError")
        << ",\"description\":" << Quote(description != nullptr ? description : "unknown runtime error") << "}";
    return out.str();
}

std::string NotAttemptedJson(const std::string& operation, const std::string& reason)
{
    std::ostringstream out;
    out << "{\"operation\":" << Quote(operation)
        << ",\"ok\":false"
        << ",\"not_attempted\":true"
        << ",\"reason\":" << Quote(reason) << "}";
    return out.str();
}

std::string DriverErrorName(CUresult result)
{
    const char* name = nullptr;
    cuGetErrorName(result, &name);
    return name != nullptr ? name : "CUDA_ERROR";
}

std::string DriverErrorDescription(CUresult result)
{
    const char* desc = nullptr;
    cuGetErrorString(result, &desc);
    return desc != nullptr ? desc : "unknown driver error";
}

std::string DriverResultJson(CUresult result, const std::string& operation)
{
    std::ostringstream out;
    out << "{\"operation\":" << Quote(operation)
        << ",\"ok\":" << Bool(result == CUDA_SUCCESS)
        << ",\"code\":" << static_cast<int>(result)
        << ",\"name\":" << Quote(DriverErrorName(result))
        << ",\"description\":" << Quote(DriverErrorDescription(result)) << "}";
    return out.str();
}

bool WritePayload(const std::string& path, const ProbePayload& payload, std::string& error)
{
    std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!file)
    {
        error = "failed to open payload file for writing: " + path;
        return false;
    }
    file.write(reinterpret_cast<const char*>(&payload), sizeof(payload));
    if (!file)
    {
        error = "failed to write payload file: " + path;
        return false;
    }
    return true;
}

bool ReadPayload(const std::string& path, ProbePayload& payload, std::string& error)
{
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file)
    {
        error = "failed to open payload file for reading: " + path;
        return false;
    }
    file.read(reinterpret_cast<char*>(&payload), sizeof(payload));
    if (!file)
    {
        error = "failed to read payload file: " + path;
        return false;
    }
    return true;
}

bool WriteText(const std::string& path, const std::string& text)
{
    std::ofstream file(path.c_str(), std::ios::out | std::ios::trunc);
    if (!file)
    {
        return false;
    }
    file << text;
    return static_cast<bool>(file);
}

std::string ReadText(const std::string& path)
{
    std::ifstream file(path.c_str(), std::ios::in);
    if (!file)
    {
        return "{}";
    }
    std::ostringstream out;
    out << file.rdbuf();
    std::string text = out.str();
    return text.empty() ? "{}" : text;
}

std::string MakeTempFilePath()
{
    char pattern[] = "/tmp/notch-cuda-ipc-probe-XXXXXX";
    const int fd = mkstemp(pattern);
    if (fd >= 0)
    {
        close(fd);
    }
    return std::string(pattern);
}

std::string CurrentExecutablePath()
{
    char buffer[4096];
    const ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1U);
    if (size <= 0)
    {
        return "";
    }
    buffer[size] = '\0';
    return std::string(buffer);
}

std::string DeviceAttributesJson(int deviceIndex)
{
    CUresult init = cuInit(0);
    if (init != CUDA_SUCCESS)
    {
        return "{\"cuInit\":" + DriverResultJson(init, "cuInit") + "}";
    }

    CUdevice device = 0;
    CUresult deviceResult = cuDeviceGet(&device, deviceIndex);
    if (deviceResult != CUDA_SUCCESS)
    {
        return "{\"cuDeviceGet\":" + DriverResultJson(deviceResult, "cuDeviceGet") + "}";
    }

    struct Attribute
    {
        const char* name;
        CUdevice_attribute value;
    };

    const Attribute attributes[] = {
        {"unified_addressing", CU_DEVICE_ATTRIBUTE_UNIFIED_ADDRESSING},
        {"ipc_event_supported", CU_DEVICE_ATTRIBUTE_IPC_EVENT_SUPPORTED},
        {"virtual_memory_management_supported", CU_DEVICE_ATTRIBUTE_VIRTUAL_MEMORY_MANAGEMENT_SUPPORTED},
        {"posix_file_descriptor_handle_supported", CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR_SUPPORTED},
    };

    std::ostringstream out;
    out << "{";
    for (size_t i = 0; i < sizeof(attributes) / sizeof(attributes[0]); ++i)
    {
        int value = 0;
        CUresult result = cuDeviceGetAttribute(&value, attributes[i].value, device);
        if (i > 0U)
        {
            out << ",";
        }
        out << Quote(attributes[i].name) << ":{\"ok\":" << Bool(result == CUDA_SUCCESS)
            << ",\"value\":" << value;
        if (result != CUDA_SUCCESS)
        {
            out << ",\"error\":" << DriverResultJson(result, "cuDeviceGetAttribute");
        }
        out << "}";
    }
    out << "}";
    return out.str();
}

int RunChild(const std::string& payloadPath, const std::string& resultPath)
{
    ProbePayload payload;
    std::string payloadError;
    if (!ReadPayload(payloadPath, payload, payloadError))
    {
        WriteText(resultPath, "{\"result\":\"error\",\"error\":" + Quote(payloadError) + "}\n");
        return 2;
    }

    cudaError_t setDevice = cudaSetDevice(0);
    int boundDevice = -1;
    cudaError_t getDevice = cudaGetDevice(&boundDevice);

    cudaEvent_t event = nullptr;
    cudaError_t eventOpen = cudaIpcOpenEventHandle(&event, payload.eventHandle);
    cudaError_t eventQuery = cudaErrorUnknown;
    bool eventQueryAttempted = false;
    if (eventOpen == cudaSuccess)
    {
        eventQueryAttempted = true;
        eventQuery = cudaEventQuery(event);
        cudaEventDestroy(event);
    }

    void* devicePtr = nullptr;
    cudaError_t memOpen = cudaIpcOpenMemHandle(&devicePtr, payload.memHandle, cudaIpcMemLazyEnablePeerAccess);
    cudaError_t copyResult = cudaErrorUnknown;
    cudaError_t closeResult = cudaErrorUnknown;
    bool copyAttempted = false;
    bool closeAttempted = false;
    unsigned long long actualHash = 0ULL;
    bool hashMatch = false;

    if (memOpen == cudaSuccess)
    {
        std::vector<unsigned char> bytes(payload.sizeBytes);
        copyAttempted = true;
        copyResult = cudaMemcpy(&bytes[0], devicePtr, bytes.size(), cudaMemcpyDeviceToHost);
        closeAttempted = true;
        closeResult = cudaIpcCloseMemHandle(devicePtr);
        if (copyResult == cudaSuccess)
        {
            actualHash = Fnv1a64(bytes);
            hashMatch = actualHash == payload.expectedHash;
        }
    }

    const bool pass =
        setDevice == cudaSuccess &&
        getDevice == cudaSuccess &&
        boundDevice == 0 &&
        eventOpen == cudaSuccess &&
        (eventQuery == cudaSuccess || eventQuery == cudaErrorNotReady) &&
        memOpen == cudaSuccess &&
        copyResult == cudaSuccess &&
        hashMatch;

    std::ostringstream out;
    out << "{"
        << "\"result\":" << Quote(pass ? "pass" : "fail")
        << ",\"process\":\"consumer\""
        << ",\"cudaSetDevice\":" << CudaResultJson(setDevice, "cudaSetDevice")
        << ",\"cudaGetDevice\":" << CudaResultJson(getDevice, "cudaGetDevice")
        << ",\"bound_device\":" << boundDevice
        << ",\"cudaIpcOpenEventHandle\":" << CudaResultJson(eventOpen, "cudaIpcOpenEventHandle")
        << ",\"cudaEventQuery\":"
        << (eventQueryAttempted ? CudaResultJson(eventQuery, "cudaEventQuery")
                                : NotAttemptedJson("cudaEventQuery", "cudaIpcOpenEventHandle failed"))
        << ",\"cudaIpcOpenMemHandle\":" << CudaResultJson(memOpen, "cudaIpcOpenMemHandle")
        << ",\"cudaMemcpy\":"
        << (copyAttempted ? CudaResultJson(copyResult, "cudaMemcpy")
                          : NotAttemptedJson("cudaMemcpy", "cudaIpcOpenMemHandle failed"))
        << ",\"cudaIpcCloseMemHandle\":"
        << (closeAttempted ? CudaResultJson(closeResult, "cudaIpcCloseMemHandle")
                           : NotAttemptedJson("cudaIpcCloseMemHandle", "cudaIpcOpenMemHandle failed"))
        << ",\"expected_hash_fnv1a64\":" << Quote(Hex64(payload.expectedHash))
        << ",\"actual_hash_fnv1a64\":" << Quote(Hex64(actualHash))
        << ",\"hash_match\":" << Bool(hashMatch)
        << "}\n";
    WriteText(resultPath, out.str());
    return pass ? 0 : 10;
}

std::string ParentFailureJson(const std::string& error)
{
    std::ostringstream out;
    out << "{"
        << "\"schema_version\":1"
        << ",\"probe\":\"notch_cuda_ipc_probe\""
        << ",\"result\":\"error\""
        << ",\"error\":" << Quote(error)
        << "}\n";
    return out.str();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 4 && std::string(argv[1]) == "--child")
    {
        return RunChild(argv[2], argv[3]);
    }

    int runtimeVersion = 0;
    int driverVersion = 0;
    cudaRuntimeGetVersion(&runtimeVersion);
    cudaDriverGetVersion(&driverVersion);

    int deviceCount = 0;
    cudaError_t countResult = cudaGetDeviceCount(&deviceCount);
    if (countResult != cudaSuccess || deviceCount <= 0)
    {
        std::ostringstream out;
        out << "{"
            << "\"schema_version\":1"
            << ",\"probe\":\"notch_cuda_ipc_probe\""
            << ",\"result\":\"skip\""
            << ",\"reason\":\"cuda_unavailable\""
            << ",\"cudaGetDeviceCount\":" << CudaResultJson(countResult, "cudaGetDeviceCount")
            << ",\"device_count\":" << deviceCount
            << ",\"runtime_version\":" << runtimeVersion
            << ",\"driver_version\":" << driverVersion
            << "}\n";
        std::cout << out.str();
        return 0;
    }

    cudaError_t setDevice = cudaSetDevice(0);
    if (setDevice != cudaSuccess)
    {
        std::cout << ParentFailureJson("cudaSetDevice(0) failed: " + std::string(cudaGetErrorString(setDevice)));
        return 2;
    }

    cudaDeviceProp prop;
    std::memset(&prop, 0, sizeof(prop));
    cudaError_t propResult = cudaGetDeviceProperties(&prop, 0);

    std::vector<unsigned char> pattern = MakePattern(kAllocationBytes);
    const unsigned long long expectedHash = Fnv1a64(pattern);

    void* devicePtr = nullptr;
    cudaError_t mallocResult = cudaMalloc(&devicePtr, kAllocationBytes);
    if (mallocResult != cudaSuccess)
    {
        std::cout << ParentFailureJson("cudaMalloc failed: " + std::string(cudaGetErrorString(mallocResult)));
        return 2;
    }

    cudaError_t copyResult = cudaMemcpy(devicePtr, &pattern[0], pattern.size(), cudaMemcpyHostToDevice);
    cudaEvent_t event = nullptr;
    cudaError_t eventCreate = cudaEventCreateWithFlags(&event, cudaEventDisableTiming | cudaEventInterprocess);
    cudaError_t eventRecord = cudaErrorUnknown;
    cudaError_t eventSync = cudaErrorUnknown;
    bool eventRecordAttempted = false;
    bool eventSyncAttempted = false;
    if (eventCreate == cudaSuccess)
    {
        eventRecordAttempted = true;
        eventRecord = cudaEventRecord(event, 0);
        eventSyncAttempted = true;
        eventSync = cudaEventSynchronize(event);
    }

    ProbePayload payload;
    std::memset(&payload, 0, sizeof(payload));
    payload.sizeBytes = kAllocationBytes;
    payload.expectedHash = expectedHash;
    cudaError_t getMemHandle = cudaIpcGetMemHandle(&payload.memHandle, devicePtr);
    cudaError_t getEventHandle = eventCreate == cudaSuccess
        ? cudaIpcGetEventHandle(&payload.eventHandle, event)
        : cudaErrorInvalidResourceHandle;

    std::string payloadPath = MakeTempFilePath();
    std::string resultPath = payloadPath + ".result.json";
    std::string payloadError;
    const bool payloadWritten = WritePayload(payloadPath, payload, payloadError);

    int childExit = -1;
    std::string childJson = "{}";
    if (copyResult == cudaSuccess && getMemHandle == cudaSuccess && getEventHandle == cudaSuccess && payloadWritten)
    {
        std::string exePath = CurrentExecutablePath();
        if (!exePath.empty())
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                execl(exePath.c_str(), exePath.c_str(), "--child", payloadPath.c_str(), resultPath.c_str(),
                      static_cast<char*>(nullptr));
                _exit(127);
            }
            else if (pid > 0)
            {
                int status = 0;
                waitpid(pid, &status, 0);
                if (WIFEXITED(status))
                {
                    childExit = WEXITSTATUS(status);
                }
                else if (WIFSIGNALED(status))
                {
                    childExit = 128 + WTERMSIG(status);
                }
            }
            else
            {
                childExit = 126;
            }
        }
        childJson = ReadText(resultPath);
    }

    if (event != nullptr)
    {
        cudaEventDestroy(event);
    }
    cudaFree(devicePtr);
    std::remove(payloadPath.c_str());
    std::remove(resultPath.c_str());

    const bool producerOk =
        copyResult == cudaSuccess &&
        eventCreate == cudaSuccess &&
        eventRecord == cudaSuccess &&
        eventSync == cudaSuccess &&
        getMemHandle == cudaSuccess &&
        getEventHandle == cudaSuccess &&
        payloadWritten;
    const bool pass = producerOk && childExit == 0;

    std::ostringstream out;
    out << "{"
        << "\"schema_version\":1"
        << ",\"probe\":\"notch_cuda_ipc_probe\""
        << ",\"result\":" << Quote(pass ? "pass" : "fail")
        << ",\"purpose\":\"pure C++ same-container legacy CUDA IPC memory/event repro\""
        << ",\"runtime_version\":" << runtimeVersion
        << ",\"driver_version\":" << driverVersion
        << ",\"device_count\":" << deviceCount
        << ",\"device\":{\"index\":0"
        << ",\"name\":" << Quote(propResult == cudaSuccess ? prop.name : "")
        << ",\"properties\":" << CudaResultJson(propResult, "cudaGetDeviceProperties")
        << ",\"attributes\":" << DeviceAttributesJson(0)
        << "}"
        << ",\"allocation\":{\"api\":\"cudaMalloc\",\"bytes\":" << kAllocationBytes
        << ",\"size_aligned_2mib\":true,\"managed\":false,\"async_pool\":false}"
        << ",\"producer\":{"
        << "\"cudaMemcpyHostToDevice\":" << CudaResultJson(copyResult, "cudaMemcpyHostToDevice")
        << ",\"cudaEventCreateWithFlags\":" << CudaResultJson(eventCreate, "cudaEventCreateWithFlags")
        << ",\"cudaEventRecord\":"
        << (eventRecordAttempted ? CudaResultJson(eventRecord, "cudaEventRecord")
                                 : NotAttemptedJson("cudaEventRecord", "cudaEventCreateWithFlags failed"))
        << ",\"cudaEventSynchronize\":"
        << (eventSyncAttempted ? CudaResultJson(eventSync, "cudaEventSynchronize")
                               : NotAttemptedJson("cudaEventSynchronize", "cudaEventCreateWithFlags failed"))
        << ",\"cudaIpcGetMemHandle\":" << CudaResultJson(getMemHandle, "cudaIpcGetMemHandle")
        << ",\"cudaIpcGetEventHandle\":"
        << (eventCreate == cudaSuccess ? CudaResultJson(getEventHandle, "cudaIpcGetEventHandle")
                                       : NotAttemptedJson("cudaIpcGetEventHandle", "cudaEventCreateWithFlags failed"))
        << ",\"expected_hash_fnv1a64\":" << Quote(Hex64(expectedHash))
        << ",\"payload_written\":" << Bool(payloadWritten);
    if (!payloadError.empty())
    {
        out << ",\"payload_error\":" << Quote(payloadError);
    }
    out << "}"
        << ",\"consumer_exit_code\":" << childExit
        << ",\"consumer\":" << childJson
        << "}\n";

    std::cout << out.str();
    return pass ? 0 : 10;
}
