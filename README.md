# comfy-action

Docker-based compatibility checks for `ComfyUI-Notch`.

This action is meant to be used from the `ComfyUI-Notch` repository. It builds a
local Docker image, starts a pinned ComfyUI checkout inside the container,
installs the caller's `ComfyUI-Notch` checkout into `custom_nodes`, compiles the
C++ client interface, and verifies that `/object_info` exposes the cross-platform
Notch node classes and server feature facts.

## What It Checks

`mode: extension_boot`:

- clone ComfyUI at `comfyui_ref`;
- copy or clone `ComfyUI-Notch` into `ComfyUI/custom_nodes/ComfyUI-Notch`;
- create an isolated Python virtual environment inside the container;
- install PyTorch, ComfyUI dependencies, and `ComfyUI-Notch` dependencies;
- build and run `cpp/notch_comfy_client`'s compile-check target;
- start ComfyUI on `<listen_address>:<port>` and poll the same address;
- assert `/features` includes Notch compatibility facts;
- assert `/object_info` contains `NotchSingleInput` and `NotchOutputNode`;
- write environment, compatibility, log, and result artifacts.

`mode: protocol_negotiation` runs the boot setup and then the C++ mock client's
discovery + transport-negotiation + readiness-decision cases (Layer 1, no GPU).

Real delivery testing is planned as a separate round-trip job. It will exercise
`NotchSingleInput -> execute -> NotchOutputNode -> client fetch/import -> verify`
instead of a thin queue-only check.

## Runner Model

Use a self-hosted runner with Docker installed and the Docker engine running.
For CUDA tests, install the NVIDIA driver and NVIDIA Container Toolkit so Docker
can run containers with:

```bash
docker run --gpus all ...
```

This action is a composite action that invokes Docker explicitly rather than a
pure `runs: using: docker` action. That is intentional: GitHub's Docker action
metadata does not give us a good place to request `--gpus all`, while this
wrapper can probe and pass it.

## Example Workflow

Create this in `ComfyUI-Notch` as
`.github/workflows/notch-compatibility.yml`:

```yaml
name: Notch Comfy compatibility checks

on:
  workflow_dispatch:
    inputs:
      comfyui_ref:
        description: "ComfyUI tag or commit"
        required: true
        default: "v0.23.0"

jobs:
  extension-boot:
    runs-on: [self-hosted, Windows, X64]
    steps:
      - uses: actions/checkout@v4

      - name: Run extension boot check
        uses: jKaarlehto/comfy-action@<pin-this-action-commit>
        with:
          mode: extension_boot
          comfyui_ref: ${{ inputs.comfyui_ref }}
          use_gpu: auto
```

Pin `uses:` to a commit SHA once this branch is merged. Do not leave production
checks on a floating branch.

## Inputs

| Input | Default | Meaning |
|---|---|---|
| `mode` | `extension_boot` | `extension_boot` runs the boot/node/feature/client compile check. `protocol_negotiation` adds the Layer-1 discovery + transport-negotiation + readiness cases. |
| `comfyui_repository` | `https://github.com/comfyanonymous/ComfyUI.git` | ComfyUI repository URL. |
| `comfyui_ref` | `v0.23.0` | ComfyUI tag, branch, or commit. Prefer release tags or commits for reproducible compatibility records. |
| `extension_repository` | empty | Optional `ComfyUI-Notch` repository URL. Empty means use the caller workspace checkout. |
| `extension_ref` | empty | Optional extension tag, branch, or commit when `extension_repository` is set. |
| `listen_address` | `127.0.0.1` | ComfyUI listen address inside the container. The check client polls this same address. |
| `port` | `8188` | ComfyUI port inside the container. |
| `timeout` | `180` | Seconds to wait for server startup. |
| `comfyui_flags` | `--disable-auto-launch` | Extra flags passed to `python main.py`. |
| `torch_index_url` | `https://download.pytorch.org/whl/cu130` | PyTorch pip index (NVIDIA stable, used as `--extra-index-url`). |
| `install_torch` | `true` | Install torch/torchvision/torchaudio before ComfyUI requirements. |
| `use_gpu` | `auto` | `true`, `false`, or `auto`. `auto` probes `docker run --gpus all`. |
| `docker_image` | `notch-contract-ci:local` | Local image tag. |
| `docker_no_cache` | `false` | Build with `--no-cache`. |
| `artifact_dir` | `notch-contract-artifacts` | Artifact directory under the caller workspace. |
| `pip_cache_dir` | `notch-contract-pip-cache` | Shared pip wheel cache under the runner workspace, mounted at `/cache/pip` and reused across both jobs and between runs. |
| `upload_artifacts` | `true` | Upload artifacts with `actions/upload-artifact`. |
| `expected_node_classes` | `NotchSingleInput,NotchOutputNode` | Comma-separated `/object_info` keys to assert. Spout is Windows-only and not part of the Linux Docker extension-boot expectation. |

## Artifacts

The action writes artifacts under `artifact_dir` and uploads them by default:

- `comfyui.log`
- `cpp-compile.log`
- `python-install.log`
- `git.log`
- `environment.json`
- `python-env.json`
- `pip-freeze.txt`
- `server-feature-flags.json`
- `object-info-summary.json`
- `object-info-debug.json` when node discovery fails
- `extension-boot-result.json`
- `conformance-result.json` when `mode: protocol_negotiation`
- `compatibility-result.json`

`compatibility-result.json` is the file to use when promoting a
last-known-good ComfyUI version. Promotion should be a separate manual workflow
step in the caller repo, not automatic behavior inside this action.

## Local Development

From this repo:

```bash
python -m py_compile scripts/notch_contract_ci.py
python scripts/notch_contract_ci.py --help
docker build -t notch-contract-ci:local .
```

To run the container manually against a local `ComfyUI-Notch` checkout:

```bash
mkdir -p /tmp/notch-contract-artifacts
docker run --rm \
  -v /path/to/ComfyUI-Notch:/workspace \
  -v /tmp/notch-contract-artifacts:/artifacts \
  notch-contract-ci:local \
  --mode extension_boot \
  --comfyui-repository https://github.com/comfyanonymous/ComfyUI.git \
  --comfyui-ref v0.23.0 \
  --host 127.0.0.1 \
  --port 8188 \
  --workspace /workspace \
  --artifacts /artifacts
```
