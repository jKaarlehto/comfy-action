#ifndef NOTCH_MOCK_SERVICE_SHIM_H
#define NOTCH_MOCK_SERVICE_SHIM_H

#include <string>
#include <vector>

#include "comfy_extension_client/client.hpp"

namespace notch_mock
{

// Outbound text channel the harness owns. The slim client is a protocol codec
// with no WebSocket surface, so the harness sends the feature-flags announce
// (Client::FeatureFlagsMessage) over its own socket.
class IWebSocketSender
{
public:
    virtual ~IWebSocketSender() {}
    virtual bool SendText(const std::string& message, std::string& error) = 0;
};

// Server facts from live GET /features (extension.notch). The shim parses these
// directly: the vendorable client no longer reads /features — discovery is
// Notch-service-owned, and the shim mirrors that service behavior for CI.
struct ServerFacts
{
    ServerFacts();

    ComfyExtensionClient::Version plugin_version;
    ComfyExtensionClient::Version protocol_version;
    ComfyExtensionClient::VersionRange supports_protocol;
    ComfyExtensionClient::Version minimum_client_protocol;
    ComfyExtensionClient::CapabilitySet capabilities;
    std::vector<std::string> tested_comfyui_refs;
    std::vector<std::string> output_transports;
    std::vector<std::string> named_disk_route_ids;
    std::vector<std::string> workflow_sources;
    int named_disk_route_revision;
    int cuda_device_index;
    int live_editor_default_timeout_ms;
    int live_editor_max_timeout_ms;
};

// Result of the Connect gate: discovery + protocol-range compatibility +
// the feature-flags announce.
struct Session
{
    ComfyExtensionClient::Version protocol_version;
    ComfyExtensionClient::CapabilitySet capabilities;
    ServerFacts server;
};

// One entry from POST /notch/get-required-files.
struct RequiredFile
{
    RequiredFile();

    std::string node_id;
    std::string class_type;
    std::string input_name;
    std::string category;
    std::string filename;
    std::string full_path;
    bool exists;
};

// A fetched http output artifact.
struct GeneratedResult
{
    GeneratedResult();

    ComfyExtensionClient::OutputReady output;
    int status_code;
    std::string bytes;
};

// Inputs to the transport-selection helper: the three eligibility sets plus
// either a hard required transport or a soft preference order.
struct OutputTransportRequest
{
    ComfyExtensionClient::WorkflowOutput output;
    ServerFacts server;
    std::vector<std::string> client_reachable_transports;
    std::vector<std::string> preference_order;
    std::string required_transport;
};

struct OutputTransportDecision
{
    OutputTransportDecision();

    bool ok;
    std::string transport;
    std::vector<std::string> usable_transports;
    std::string error;
};

// Pure set-based transport selection: usable = type-allowed n server-available
// n client-reachable; a hard request is honored iff usable, else rejected; a
// soft order takes the first usable, else the first usable overall. This logic
// moved from the vendorable client into the Notch service; the shim keeps the
// reference implementation the conformance matrix asserts against.
OutputTransportDecision SelectOutputTransport(const OutputTransportRequest& request);

// Service-side orchestration the conformance harness needs on top of the slim
// ComfyExtensionClient::Client codec: discovery, the protocol-compatibility
// gate, deployment readiness, submission, and http artifact fetching. In the
// product these behaviors live in the native Notch service; the shim mirrors
// them so the suite still exercises the server endpoints end to end.
class ServiceShim
{
public:
    ServiceShim(
        ComfyExtensionClient::Client& client,
        ComfyExtensionClient::HttpTransport& http,
        IWebSocketSender* webSocket);

    ComfyExtensionClient::Result<ServerFacts> Discover();
    ComfyExtensionClient::Result<Session> Connect();
    ComfyExtensionClient::Result<bool> SendFeatureFlags();
    ComfyExtensionClient::Result<std::vector<RequiredFile> > GetRequiredFiles(const std::string& workflowJson);
    ComfyExtensionClient::Result<ComfyExtensionClient::JobHandle> Submit(
        const ComfyExtensionClient::GenerateRequest& request);
    ComfyExtensionClient::Result<GeneratedResult> FetchOutput(
        const ComfyExtensionClient::OutputReady& output) const;

private:
    ComfyExtensionClient::Client& m_client;
    ComfyExtensionClient::HttpTransport& m_http;
    IWebSocketSender* m_webSocket;
};

} // namespace notch_mock

#endif
