cmake_minimum_required(VERSION 3.15)

project(notch_mock_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Path to the checked-out client interface (cpp/notch_comfy_client). The runner
# points this at the ComfyUI-Notch copy under test so the mock client links the
# *current* interface and any drift surfaces as a build failure.
if(NOT DEFINED NOTCH_COMFY_CLIENT_DIR)
    message(FATAL_ERROR "NOTCH_COMFY_CLIENT_DIR must point at cpp/notch_comfy_client")
endif()
if(NOT EXISTS "${NOTCH_COMFY_CLIENT_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "NOTCH_COMFY_CLIENT_DIR has no CMakeLists.txt: ${NOTCH_COMFY_CLIENT_DIR}")
endif()
add_subdirectory(${NOTCH_COMFY_CLIENT_DIR} notch_comfy_client_build)

# Single third-party transport dependency: IXWebSocket gives both an HTTP client
# and a WebSocket client. Local CI is plaintext, so TLS and zlib are disabled to
# keep the image dependency set minimal.
include(FetchContent)
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v11.4.5
)
FetchContent_MakeAvailable(ixwebsocket)

# IXWebSocket is a vendored third-party dependency; silence its own compiler
# warnings (e.g. -Wunused-parameter in its deflate codec) so the build log shows
# only warnings from our code, not noise from the dependency.
if(TARGET ixwebsocket)
    target_compile_options(ixwebsocket PRIVATE -w)
endif()

find_package(CUDAToolkit QUIET)

# Transport-agnostic contract logic. Depends only on the client interface, so it
# can be reused and self-tested without the transport library.
add_library(notch_mock_matrix STATIC
    src/matrix.cpp
    src/case_logger.cpp
    src/hash_utils.cpp
    src/delivery_types.cpp
    src/verify/verify_byte_exact.cpp
    src/verify/verify_integrity.cpp
    src/verify/verify_structural.cpp
)
target_include_directories(notch_mock_matrix PUBLIC src)
target_link_libraries(notch_mock_matrix PUBLIC notch_comfy_client)

# The CI executable: wires the IXWebSocket transports into the matrix.
add_executable(notch_mock_client
    src/main.cpp
    src/transports/local_http_transport.cpp
    src/transports/local_websocket_transport.cpp
)
target_link_libraries(notch_mock_client PRIVATE notch_mock_matrix ixwebsocket)
if(CUDAToolkit_FOUND)
    target_sources(notch_mock_client PRIVATE src/transports/local_cuda_share_reader.cpp)
    # Runtime API (cudart) is the production import path the Notch client uses;
    # the driver library (cuda_driver) provides cuIpcOpenMemHandle for the
    # reader's fallback open when the runtime open returns InvalidResourceHandle.
    target_link_libraries(notch_mock_client PRIVATE CUDA::cudart CUDA::cuda_driver)
    target_compile_definitions(notch_mock_client PRIVATE NOTCH_MOCK_HAS_CUDA_RUNTIME=1)

    if(UNIX)
        add_executable(notch_cuda_ipc_probe src/tools/cuda_ipc_probe.cpp)
        target_link_libraries(notch_cuda_ipc_probe PRIVATE CUDA::cudart CUDA::cuda_driver)
    endif()
endif()

# Transport-agnostic self-test: exercises the matrix with stub transports, no
# network and no IXWebSocket. Runs as part of the build verification.
enable_testing()
add_executable(notch_mock_stub_check tests/stub_check.cpp)
target_link_libraries(notch_mock_stub_check PRIVATE notch_mock_matrix)
add_test(NAME notch_mock_stub_check COMMAND notch_mock_stub_check)

add_executable(notch_mock_verify_check tests/verify_check.cpp)
target_link_libraries(notch_mock_verify_check PRIVATE notch_mock_matrix)
add_test(NAME notch_mock_verify_check COMMAND notch_mock_verify_check)
