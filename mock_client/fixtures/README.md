# Conformance fixtures

Small API-format workflows fed to the mock client for the live (non-stub) run.
The C++ interface wraps each one in `{"prompt": ...}` before sending, so these
files are the bare graph object. Every node needs a `_meta` field — ComfyUI's
API schema validation (`/notch/parse`) requires it.

## parse_workflow.json — type-axis discovery
Two `NotchOutputNode`s, one wired from an `image` input, one from a `mesh`
input. `/notch/parse` reports `outputs[].transports` per type:
- IMAGE → `cuda,disk,http`
- MESH  → `disk,http`
The upstream source node type is irrelevant — `NotchOutputNode` uses
`accept_all_inputs`, and the reported output type comes from the wired input
name, not the source.

## required_files_ready.json — readiness gate, positive
A workflow with **no file-backed inputs** (`EmptyLatentImage`). The manifest is
empty → 0 missing → ready. This is the honest positive in a model-free runner:
we cannot reference a "present" model when no models are installed.

## required_files_missing.json — readiness gate, negative
References `CheckpointLoaderSimple` with an absent checkpoint. Expected:
exists=false → 1 missing → not-ready.

**Coordination note:** this case passes only once the extension's file-manifest
registry detects file-backed inputs in a model-free install (empty model
folders). The original registry classified an input as file-backed only when its
COMBO list was non-empty and intersected installed files, so with empty folders
the missing checkpoint was invisible and this case reported a false "ready". The
fix (capturing the `folder_paths` category binding by intercepting
`get_filename_list` during `INPUT_TYPES()`, rather than inferring it from
contents) makes the empty-folder case correct. Until that fix is on the
ComfyUI-Notch ref under test, `deployment-missing-file` is expected to fail.

Delivery-local and delivery-remote workflows are generated in C++ by the mock
client so each case can stamp its own consumer id, selected transport, and
output path while still using `ClientProtocol::BuildWorkflowSubmissionRequest`.
