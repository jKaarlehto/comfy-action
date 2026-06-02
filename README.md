# comfy-action

Docker-based contract checks for `ComfyUI-Notch`.

This action is meant to be used from the `ComfyUI-Notch` repository. It builds a
local Docker image, starts a pinned ComfyUI checkout inside the container,
installs the caller's `ComfyUI-Notch` checkout into `custom_nodes`, compiles the
C++ client interface, and verifies that `/object_info` exposes the Notch node
classes.

## What It Checks

`mode: smoke`:

- clone ComfyUI at `comfyui_ref`;
- copy or clone `ComfyUI-Notch` into `ComfyUI/custom_nodes/ComfyUI-Notch`;
- create an isolated Python virtual environment inside the container;
- install PyTorch, ComfyUI dependencies, and `ComfyUI-Notch` dependencies;
- build and run `cpp/notch_comfy_client`'s compile-check target;
- start ComfyUI on `<listen_address>:<port>` and poll the same address;
- assert `/object_info` contains `NotchSingleInput`, `NotchOutputNode`, and `SpoutReceiver`;
- write environment, compatibility, log, and result artifacts.

`mode: integration` is reserved for the next phase. It currently runs the smoke
path and then fails with a clear message instead of claiming the live
HTTP/WS/CUDA permutation suite exists.

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
`.github/workflows/notch-contract-smoke.yml`:

```yaml
name: notch contract smoke

on:
  workflow_dispatch:
    inputs:
      comfyui_ref:
        description: "ComfyUI tag or commit"
        required: true
        default: "v0.23.0"

jobs:
  smoke:
    runs-on: [self-hosted, comfy-contract]
    steps:
      - uses: actions/checkout@v4

      - name: Run contract smoke
        uses: jKaarlehto/comfy-action@<pin-this-action-commit>
        with:
          mode: smoke
          comfyui_ref: ${{ inputs.comfyui_ref }}
          use_gpu: auto
```

Pin `uses:` to a commit SHA once this branch is merged. Do not leave production
checks on a floating branch.

## Inputs

| Input | Default | Meaning |
|---|---|---|
| `mode` | `smoke` | `smoke` runs the implemented check. `integration` is reserved for the future mock-client suite and currently fails after smoke. |
| `comfyui_repository` | `https://github.com/comfyanonymous/ComfyUI.git` | ComfyUI repository URL. |
| `comfyui_ref` | `v0.23.0` | ComfyUI tag, branch, or commit. Prefer release tags or commits for reproducible compatibility records. |
| `extension_repository` | empty | Optional `ComfyUI-Notch` repository URL. Empty means use the caller workspace checkout. |
| `extension_ref` | empty | Optional extension tag, branch, or commit when `extension_repository` is set. |
| `listen_address` | `127.0.0.1` | ComfyUI listen address inside the container. The smoke client polls this same address. |
| `port` | `8188` | ComfyUI port inside the container. |
| `timeout` | `180` | Seconds to wait for server startup. |
| `comfyui_flags` | `--disable-auto-launch` | Extra flags passed to `python main.py`. |
| `torch_index_url` | `https://download.pytorch.org/whl/cu121` | PyTorch pip index. |
| `install_torch` | `true` | Install torch/torchvision/torchaudio before ComfyUI requirements. |
| `use_gpu` | `auto` | `true`, `false`, or `auto`. `auto` probes `docker run --gpus all`. |
| `docker_image` | `notch-contract-ci:local` | Local image tag. |
| `docker_no_cache` | `false` | Build with `--no-cache`. |
| `artifact_dir` | `notch-contract-artifacts` | Artifact directory under the caller workspace. |
| `upload_artifacts` | `true` | Upload artifacts with `actions/upload-artifact`. |
| `expected_node_classes` | `NotchSingleInput,NotchOutputNode,SpoutReceiver` | Comma-separated `/object_info` keys to assert. |

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
- `smoke-result.json`
- `integration-result.json` when `mode: integration`
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
  --mode smoke \
  --comfyui-repository https://github.com/comfyanonymous/ComfyUI.git \
  --comfyui-ref v0.23.0 \
  --host 127.0.0.1 \
  --port 8188 \
  --workspace /workspace \
  --artifacts /artifacts
```
