#ifndef NOTCH_MOCK_MATRIX_H
#define NOTCH_MOCK_MATRIX_H

#include <string>
#include <vector>

#include "notch_comfy_client/client_interface.h"

#include "case_logger.h"

namespace notch_mock
{

// Minimal WebSocket probe used by the matrix v1 handshake smoke case. It is a
// superset of the interface's IWebSocketTransport (which only sends) so the
// orchestration can connect, send the feature-flags message, and drain received
// frames for evidence without depending on the concrete transport library.
class IWebSocketProbe
{
public:
    virtual ~IWebSocketProbe() {}
    virtual bool Connect(int timeoutMs, std::string& error) = 0;
    virtual bool SendText(const std::string& message, std::string& error) = 0;
    virtual std::vector<std::string> DrainReceived() = 0;
    virtual void Close() = 0;
};

// Which conformance phase(s) to run. Negotiation is Layer 1 (discovery +
// transport selection + readiness decision, no execution, no GPU). Delivery is
// Layer 2 (inject + execute + delivery + readiness enforcement). All runs both.
enum class Phase
{
    Negotiation,
    Delivery,
    All,
};

struct MatrixOptions
{
    Phase phase = Phase::Negotiation;
    std::string baseUrl;           // http://127.0.0.1:8188
    std::string outputRoot;        // artifact directory root
    std::string clientId;          // must match the /ws clientId for event routing
    std::string parseWorkflowJson; // optional API workflow body for /notch/parse
    // Deployment-readiness gate fixtures (POST /notch/get-required-files): a
    // workflow whose referenced files all exist on the server, and one that
    // references a missing file. Empty fixtures skip the gate.
    std::string requiredFilesReadyJson;
    std::string requiredFilesMissingJson;
    // Delivery-phase execution fixtures: a workflow that must execute
    // successfully, and one referencing a missing file whose run must be blocked
    // (the file-availability enforcement). Empty fixtures skip those cases.
    std::string executeWorkflowJson;
    std::string executeMissingFileJson;
    int wsTimeoutMs = 5000;        // handshake budget for the smoke case
    int executeTimeoutMs = 90000;  // budget to wait for a terminal execution event
};

struct MatrixSummary
{
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    int errored = 0;
    // Set when shared setup itself failed; the matrix aborts before cases run.
    std::string setupFailureCode;

    bool Ok() const { return failed == 0 && errored == 0 && setupFailureCode.empty(); }
};

// Run the conformance suite for the selected phase. Negotiation runs discovery
// (live /features and optional /notch/parse), the readiness decision, the pure
// transport-selection matrix, and a WebSocket handshake smoke. Delivery runs the
// execution/delivery cases (inject, terminal WS event, file-availability
// enforcement) with per-case diagnostics. Diagnostic-first: every runnable case
// is executed; only a shared-setup failure short-circuits. Writes
// conformance-result.json under options.outputRoot.
MatrixSummary RunConformance(
    notch_comfy::IHttpTransport& http,
    IWebSocketProbe& ws,
    const MatrixOptions& options,
    CaseLogger& logger);

} // namespace notch_mock

#endif
