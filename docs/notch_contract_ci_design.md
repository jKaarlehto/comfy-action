# Notch Contract CI Design

This document describes the first Notch-specific version of `comfy-action`.
It adapts the action from "run a sample ComfyUI workflow" into a slim contract
check for `ComfyUI-Notch`: build a Docker runner image, start a pinned ComfyUI
version with the custom node installed, compile the C++ client interface, and
later exercise the local transport negotiation matrix.

## Ownership Model

`comfy-action` is external CI tooling. Do not add it as a submodule of
`ComfyUI-Notch`, and do not copy action source into the extension repo.

Development layout:

```text
comfy_dev/
  comfy-action/
  ComfyUI/custom_nodes/ComfyUI-Notch/
```

Develop this first version on:

```text
feat/notch-contract-ci-v1
```

When the first version is complete and reviewed, fast-forward merge that branch
to the fork's `main`. Workflows that consume the action should pin a commit SHA,
not a floating branch:

```yaml
uses: jKaarlehto/comfy-action@<commit-sha>
```

The extension repo owns the protocol and source contracts. This action owns the
runner behavior and the test orchestration that verifies those contracts.

## Runner Model

The action is a composite GitHub Action that builds and runs a local Docker
image. It is not declared as `runs: using: docker`, because the wrapper needs to
decide at runtime whether to pass `--gpus all`, mount the caller checkout, and
upload artifacts with `actions/upload-artifact`.

The host runner can be Windows or Linux as long as it has Docker and PowerShell.
During development the expected host is a self-hosted Windows runner using
Docker Desktop or a comparable Docker engine. The actual ComfyUI server, Python
environment, and C++ compile check run inside the Linux CUDA image, so the
extension-initialization contract is portable across Docker hosts.

The container mounts:

```text
<caller workspace>       -> /workspace
<action checkout>        -> /action:ro
<caller artifact dir>    -> /artifacts
```

Everything else under `/work/notch-contract-ci` is run-owned container state and
is recreated every run.

## Phase-1 Tradeoffs

This first version favors simple, inspectable local development over a polished
CI product:

- The CUDA base image is pinned by tag, not digest. Pin a digest or publish a
  versioned runner image once the dependency set stabilizes.
- The image is built every run. This is slower, but keeps the dev path obvious;
  add BuildKit cache or a prebuilt image later if build time becomes noise.
- The self-hosted runner is trusted infrastructure. Docker gives environment
  repeatability and cleanup, but it is not a security boundary for untrusted
  workflows.
- `contract_matrix` intentionally fails until the C++ mock client exists. The
  action must not report a green matrix contract before that suite is real.

## Goals

- Pin each run to an explicit ComfyUI release tag, branch, or commit.
- Verify that ComfyUI starts with `ComfyUI-Notch` installed.
- Verify that `/object_info` contains the cross-platform Notch node classes.
- Verify that `cpp/notch_comfy_client` compiles.
- Record the Python, CUDA, Docker-host, ComfyUI, and extension facts needed to
  reproduce a successful or failed run.
- In future `contract_matrix` mode, verify negotiation-axis positives and negatives.
- Keep a record of the last known working ComfyUI tag and runner environment.

## Non-Goals

- Do not run a Notch executable. The action tests the Comfy extension and the
  C++ client-extension contract, not native Notch output handling.
- Do not test Notch output targets. Connected, Project Resource, New Output
  Node, Save to Disk, and other target choices are Notch-side output handling
  policy, not part of transport negotiation.
- Do not emulate a remote Comfy server in the first `contract_matrix` mode. Remote
  behavior can be added later with a second container or machine.
- Do not download large model sets for the extension-initialization check.
- Do not archive an entire Python environment unless a repro needs it. Prefer
  manifest artifacts.

## Inputs

Keep configuration small:

| Input | Default | Meaning |
|---|---|---|
| `mode` | `extension_initialization` | `extension_initialization` runs the implemented boot/node/feature/client compile check. `contract_matrix` is reserved for the future mock-client permutation suite and currently fails after initialization. |
| `comfyui_repository` | `https://github.com/comfyanonymous/ComfyUI.git` | ComfyUI repository to clone inside the container. |
| `comfyui_ref` | `v0.23.0` | ComfyUI tag, branch, or commit. Prefer tags or commits for compatibility records. |
| `extension_repository` | empty | Optional ComfyUI-Notch repository. Empty means copy the caller workspace checkout. |
| `extension_ref` | empty | Tag, branch, or commit to checkout when `extension_repository` is set. |
| `listen_address` | `127.0.0.1` | ComfyUI listen address inside the container. The check client polls this same address. |
| `port` | `8188` | Local ComfyUI HTTP port inside the container. |
| `timeout` | `180` | Seconds to wait for ComfyUI to become reachable. |
| `comfyui_flags` | `--disable-auto-launch` | Extra flags passed to `main.py`. |
| `torch_index_url` | `https://download.pytorch.org/whl/cu121` | PyTorch pip index URL used inside the container. |
| `install_torch` | `true` | Install Torch before ComfyUI requirements. |
| `use_gpu` | `auto` | `true`, `false`, or `auto`. `auto` probes `docker run --gpus all`. |
| `docker_image` | `notch-contract-ci:local` | Local Docker image tag for the runner. |
| `docker_no_cache` | `false` | Build the image with `--no-cache`. |
| `artifact_dir` | `notch-contract-artifacts` | Simple path under the caller workspace. Cleared at the start of each run. |
| `upload_artifacts` | `true` | Upload `artifact_dir` with `actions/upload-artifact`. |
| `expected_node_classes` | `NotchSingleInput,NotchOutputNode` | Comma-separated class names expected in `/object_info`. Spout is Windows-only and excluded from the Linux Docker extension-initialization expectation. |

Do not add arbitrary per-test selectors until the two modes prove too coarse.
The contract matrix suite should own its case table in source control so a run is
reproducible from `mode + comfyui_ref + extension_ref + action commit`.

## Extension Initialization Flow

1. Build the local Docker runner image from this action repo.
2. Start the runner container with the caller workspace and artifact directory
   mounted.
3. Clone the configured ComfyUI repository at `comfyui_ref` into a fresh
   container work directory.
4. Copy the caller workspace checkout of `ComfyUI-Notch`, or clone
   `extension_repository` at `extension_ref`, into:

   ```text
   ComfyUI/custom_nodes/ComfyUI-Notch
   ```

5. Create a fresh Python virtual environment inside the container.
6. Install Torch, ComfyUI requirements, and the custom node requirements.
7. Write `pip-freeze.txt` and `python-env.json`.
8. Build and run the C++ compile-check target:

   ```text
   cpp/notch_comfy_client -> notch_comfy_client_compile_check
   ```

9. Start ComfyUI on `<listen_address>:<port>` using
   `python main.py --listen=<listen_address> --port=<port>`.
10. Poll `GET /queue` until the server is reachable.
11. Best-effort collect Comfy feature flags from `GET /features`.
12. Request `GET /object_info`.
13. Assert that object info contains:

   ```text
   NotchSingleInput
   NotchOutputNode
   ```

14. Write the extension-initialization result and compatibility artifacts.
15. Stop ComfyUI; Docker removes the container.

This proves that the pinned ComfyUI version can import the extension, expose the
Comfy node classes, and compile the C++ client interface. It does not prove
workflow execution correctness.

## Contract Matrix Mode

Contract matrix mode is planned, not implemented in the first Docker action. The
current `contract_matrix` mode runs extension-initialization setup, writes a
`contract-matrix-result.json` with `result: "not_implemented"`, and fails instead
of claiming a pass.

The future matrix client should be a lightweight C++ executable built
during the run. It links the vendored
`ComfyUI-Notch/cpp/notch_comfy_client` source and provides only test transports:

- HTTP requests to `/notch/parse`, `/notch/inject`, and artifact GET routes.
- WebSocket reads for Comfy execution lifecycle, `notch-output-ready`, and
  `notch-cuda-share-status`.
- Optional CUDA Driver IPC import for CUDA-positive cases.
- Local output validation and artifact writing for CI evidence.

Keep this mock client in the action/test harness, not in
`cpp/notch_comfy_client`. The client interface remains bring-your-own-transport;
the mock client is one consumer used by CI.

The suite tests three negotiation axes:

- **Workflow/type axis:** the output's `outputs[].transports` from
  `/notch/parse`.
- **Server availability axis:** `extension.notch.output_transports` and related
  server facts from Comfy feature flags.
- **Client reachability axis:** local facts supplied by the test client.

Output target is not a negotiation axis. It is the Notch-side decision about
what to do with a delivered output after the client-extension transport contract
has succeeded.

Formal rule:

```text
usable = typeAllowedTransports
       intersection serverAvailableTransports
       intersection clientReachableTransports
```

A hard requested transport is selected only when it is in `usable`; otherwise
selection fails. A soft preference order chooses the first usable transport in
that ordered list, and if none match, the helper falls back to the first usable
transport. Matrix rows that assert "no silent downgrade" should use a hard
transport request.

Source-of-truth implementation points in `ComfyUI-Notch`:

- `core.data_types.OUTPUT_TYPES_BY_TRANSPORT`: `cuda` is image-only; `disk` and
  `http` support the artifact/output type set.
- `services.notch_workflow_graph.get_workflow_outputs(...)`: `/notch/parse`
  reports connected-output delivery transports from `cuda`, `disk`, and `http`;
  it does not report `noop`.
- `services.server_capabilities.supported_output_transports()`: feature flags
  publish `disk`, `http`, and `noop`, plus `cuda` only when CUDA is available.
- `services.output_config_service.build_notch_output_config(...)`: the server
  hard-errors type/transport mismatches and unavailable CUDA requests.
- `cpp/notch_comfy_client/src/client_interface.cpp`: the C++ helper performs the
  set intersection and hard requested-transport check.

`noop` is deliberately outside the connected-output permutation matrix. It may
appear in server feature flags because the node has an internal no-output mode,
but `/notch/inject` does not accept `noop` as an output delivery transport and
`/notch/parse` never reports it for a connected `NotchOutputNode`.

Live `/parse` type-axis assertions:

| Output type | Expected `outputs[].transports` |
|---|---|
| `IMAGE` | `cuda,disk,http` |
| `FILE_3D_GLB` and other `FILE_3D_*` subtypes | `disk,http` |
| `FILE_3D` | `disk,http` |
| `FILE_PATH` | `disk,http` |
| `AUDIO`, `VIDEO`, `MESH`, `LOAD3D_CAMERA` | `disk,http` |

Pure selection matrix, using a hard requested transport:

| Case | Type-allowed | Server-available | Client-reachable | Requested | Usable set | Expected |
|---|---|---|---|---|---|---|
| image cuda local | `cuda,disk,http` | `cuda,disk,http` | `cuda,disk,http` | `cuda` | `cuda,disk,http` | choose `cuda` |
| image disk without CUDA | `cuda,disk,http` | `disk,http` | `disk,http` | `disk` | `disk,http` | choose `disk` |
| image HTTP-only client | `cuda,disk,http` | `disk,http` | `http` | `http` | `http` | choose `http` |
| non-image rejects CUDA by type | `disk,http` | `cuda,disk,http` | `cuda,disk,http` | `cuda` | `disk,http` | reject requested `cuda` |
| non-image disk local | `disk,http` | `disk,http` | `disk,http` | `disk` | `disk,http` | choose `disk` |
| non-image rejects unreachable disk | `disk,http` | `disk,http` | `http` | `disk` | `http` | reject requested `disk` |
| file path HTTP-only client | `disk,http` | `disk,http` | `http` | `http` | `http` | choose `http` |
| image rejects unavailable CUDA | `cuda,disk,http` | `disk,http` | `cuda,disk,http` | `cuda` | `disk,http` | reject requested `cuda` |
| empty intersection fails | `disk,http` | `cuda` | `cuda` | `cuda` | empty | reject: no usable transport |

Pure selection matrix, using a soft preference order:

| Case | Type-allowed | Server-available | Client-reachable | Preference order | Usable set | Expected |
|---|---|---|---|---|---|---|
| prefer cuda then disk | `cuda,disk,http` | `disk,http` | `disk,http` | `cuda,disk,http` | `disk,http` | choose `disk` |
| no order match but usable remains | `disk,http` | `disk,http` | `http` | `cuda,disk` | `http` | choose first usable `http` |

The hard-request rows are the contract for user- or target-driven
transport choices. The soft-order rows are helper behavior only; they must not
be read as permission for the server to downgrade a submitted
`config.output.transport`.

Remote server cases are out of scope for the first `contract_matrix` mode. A future
phase can emulate remote topology by starting a second container or machine.
That should be a separate remote-topology mode because Docker networking
and filesystem mounts add their own failure modes.

### Contract Matrix Execution Model

The matrix runner must be diagnostic first: it should complete the full case
list and report every failure in one run. A single broken case must not abort the
remaining cases unless the shared setup itself failed (server cannot start,
client cannot compile, `/features` is unreachable, or `/object_info` is missing
required node classes).

Use two implementation phases:

1. **Matrix v1: discovery and selection.** Use live `/features` and
   `/notch/parse` responses plus the C++ client's transport-selection helper.
   Do not queue workflow execution yet. This verifies that the extension's
   advertised workflow/type axis, server deployment facts, and client
   reachability negotiation agree.
2. **Matrix v2: execution and delivery.** Add `/notch/inject`, WebSocket
   lifecycle capture, disk output validation, HTTP artifact fetch validation,
   CUDA share-status validation, and optional CUDA IPC import when the runner
   environment supports it.

For both phases, each case should produce a structured result object:

```json
{
  "case_id": "image-cuda-local",
  "phase": "selection",
  "expected": {"ok": true, "transport": "cuda"},
  "actual": {"ok": true, "transport": "cuda"},
  "result": "pass",
  "errors": []
}
```

Allowed case results:

- `pass`: expected and actual behavior match.
- `fail`: expected and actual behavior differ.
- `skip`: the case is not applicable to this runner, with an explicit reason
  such as `cuda_unavailable`.
- `error`: the case could not complete because the harness, server, or client
  encountered an unexpected failure.

The final `contract-matrix-result.json` should fail the job when any case is
`fail` or `error`, but only after all runnable cases have completed. It should
include totals by phase and result, plus a compact list of failed case ids and
error codes.

### Matrix Evidence And Hashes

Every case must write enough evidence to debug failures without replaying the
run. Use one directory per case:

```text
contract-matrix/
  cases/
    001-image-cuda-local/
      case.json
      mock-client.jsonl
      http.jsonl
      websocket.jsonl
      server.log
      hashes.json
      request-parse.json
      response-parse.json
      request-inject.json
      response-inject.json
```

All transcripts should be JSONL with timestamped records. Plain text may be
included for readability, but JSONL is the source of truth for automated
inspection.

- `mock-client.jsonl`: client decisions, selected transport, expected outcome,
  and validation steps.
- `http.jsonl`: sanitized HTTP request/response metadata and bodies for Notch
  API calls. Large binary bodies should be represented by size, content type,
  and hash rather than embedded bytes.
- `websocket.jsonl`: sent and received WebSocket messages, including timestamps,
  prompt ids, output-ready events, CUDA share-status messages, and terminal
  execution events.
- `server.log`: the ComfyUI log slice for the case. Keep full `comfyui.log` for
  the run, but also record per-case slices by timestamp or byte offset.
- `hashes.json`: all input and output integrity checks for the case.

Hash all deterministic test inputs before sending them and all observed outputs
after receiving them. Prefer SHA-256 over ad hoc checksums. `hashes.json` should
record at least:

```json
{
  "inputs": [
    {"name": "image", "type": "IMAGE", "bytes": 1048576, "sha256": "<hex>"}
  ],
  "requests": [
    {"name": "parse_request", "sha256": "<canonical-json-sha256>"}
  ],
  "outputs": [
    {"name": "notch-output-ready", "transport": "http", "bytes": 1048576, "sha256": "<hex>"}
  ],
  "expected_outputs": [
    {"name": "image", "sha256": "<hex>", "comparison": "exact"}
  ]
}
```

Use exact hashes for byte-stable artifacts such as generated fixture files,
HTTP artifact bytes, copied file outputs, and replay metadata. For image tensor
outputs that may pass through codec conversion, use a two-part check: hash the
raw delivered artifact bytes and also validate decoded dimensions, channel
count, dtype/range, and a small deterministic pixel/sample hash when the format
is expected to preserve values. Do not mark a delivery case as pass merely
because an output file exists.

The mock client should generate deterministic payloads with embedded case ids
where practical. This makes corruption obvious in both hashes and manual
inspection. Example: small PNG/checker image, short WAV tone, tiny JSON camera
payload, tiny GLB/mesh fixture, and a plain file payload whose content includes
the case id and expected transport.

Failure categories should be stable strings so reports can be grouped:

- `parse_contract_mismatch`
- `server_feature_mismatch`
- `transport_selection_mismatch`
- `unexpected_accept`
- `unexpected_reject`
- `websocket_timeout`
- `output_event_missing`
- `output_artifact_missing`
- `output_hash_mismatch`
- `cuda_status_missing`
- `cuda_import_failed`
- `harness_error`

## Artifacts

Every run uploads an artifact directory. Prefer plain files:

```text
environment.json
python-env.json
pip-freeze.txt
server-feature-flags.json
object-info-summary.json
object-info-debug.json
comfyui.log
git.log
python-install.log
environment.log
cpp-compile.log
extension-initialization-result.json
contract-matrix-result.json
compatibility-result.json
```

`environment.json` should include:

- Container OS, CPU architecture, and Python facts.
- Host GPU visibility inside the container, including `nvidia-smi` output when
  available.
- Torch, TorchVision, and Torchaudio versions when installed.
- Torch CUDA availability and CUDA version reported by Torch.
- cuda-python import availability when installed.
- ComfyUI repository, requested ref, and resolved commit.
- ComfyUI-Notch repository/ref when cloned separately, plus resolved commit
  when available.
- C++ client-interface version and protocol version when published by feature
  flags.
- CMake and Git versions.
- Server URL and port.
- Selected mode.
- Extension-initialization assertions and result.
- Contract matrix case summary when `mode=contract_matrix`.

Every run should also write `compatibility-result.json`:

```json
{
  "schema_version": 1,
  "profile": "extension_initialization",
  "result": "pass",
  "comfyui_ref": "v0.x.y",
  "comfyui_commit": "<resolved commit>",
  "comfyui_notch_commit": "<commit>",
  "environment_snapshot_sha256": "<hash>",
  "runner": {
    "os": "Linux",
    "platform": "<container platform string>"
  },
  "checks": {
    "server_started": true,
    "queue_reachable": true,
    "object_info_contains_notch_nodes": true,
    "cpp_client_compiles": true
  }
}
```

For contract matrix mode, use `profile: "contract_matrix"` and include case totals.

## Cleanup And Caching

Self-hosted runners are stateful machines, so the action must be deliberate
about run-owned files.

Clean every run:

- The caller artifact directory selected by `artifact_dir`.
- The container `/work/notch-contract-ci` directory.
- ComfyUI checkout/worktree for the run.
- Installed `custom_nodes/ComfyUI-Notch` copy.
- Python virtual environment.
- C++ build directory.
- ComfyUI `input`, `output`, `temp`, and test artifact folders used by the run.
- Started ComfyUI process and any child process created by the workflow.

Safe to cache or reuse:

- Docker image layers.
- Pip wheel/download cache in a later phase, if the workflow records exact
  package manifests and the cache key includes the relevant lock inputs.
- Optional model cache for future non-initialization tests, but not the phase-1
  extension-initialization check.

Do not cache:

- The ComfyUI checkout itself as mutable state. Checkout the requested
  `comfyui_ref` into a fresh run directory.
- The custom node install directory. Install or copy from the checked-out repo
  each run.
- ComfyUI `output`, `temp`, `input`, or `user` state unless explicitly uploaded
  as an artifact.
- C++ build outputs as authoritative test inputs. Rebuild from source each run.

Default policy: build and install from source every run; cache only Docker image
layers unless a later phase needs package download caches.

## Last-Known-Good Record

Keep a small last-known-good compatibility record, promoted deliberately rather
than rewritten by ordinary PR runs.

Suggested path in the consuming repo:

```text
compatibility/last-known-good-comfy.json
```

Suggested content:

```json
{
  "schema_version": 1,
  "profile": "extension_initialization",
  "comfyui_ref": "v0.x.y",
  "comfyui_commit": "<resolved commit>",
  "comfyui_notch_commit": "<commit>",
  "environment_snapshot_sha256": "<hash>",
  "runner_summary": {
    "os": "Linux",
    "python": "3.12.x",
    "torch": "<version>",
    "cuda_available": false
  },
  "github_run_url": "<url>"
}
```

Promotion rules:

- PR runs may upload compatibility artifacts but must not update the
  last-known-good record.
- Manual trusted runs may promote when a consuming workflow explicitly performs
  that write.
- Promotion should require a passing result for the selected mode.
- Future contract-matrix results should have their own profile value rather than
  overwriting an extension-initialization record with stronger claims.

## Future Phases

1. Add the C++ mock client for `/notch/parse`, `/notch/inject`, WebSocket
   completion, HTTP output fetch, and CUDA import.
2. Add model-free execution tests.
3. Add disk and HTTP output artifact tests.
4. Add CUDA delivery tests when the runner can pass `--gpus all`.
5. Add remote topology tests with a second container or machine.
6. Add the full type/transport matrix when the case list justifies it.

Future matrix tests should still exclude Notch output targets. They should test
the client-extension delivery contract: transport selection, request building,
execution completion, output-ready events, HTTP artifact fetch, and CUDA share
status.
