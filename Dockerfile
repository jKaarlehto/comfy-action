# CUDA 13 base to match the ComfyUI NVIDIA stable install (torch cu130). Pin the
# exact patch tag to one available on your registry / supported by the runner
# driver; CUDA 13 requires a recent NVIDIA driver on the self-hosted host.
FROM nvidia/cuda:13.0.1-cudnn-devel-ubuntu24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV PYTHONUNBUFFERED=1
ENV PIP_DISABLE_PIP_VERSION_CHECK=1
# Reuse downloaded wheels across both jobs via the /cache/pip bind mount the
# action provides. If the mount is absent the cache is just container-local.
ENV PIP_CACHE_DIR=/cache/pip

# Python 3.13 is ComfyUI's recommended interpreter; install it from deadsnakes
# (Ubuntu 24.04 ships 3.12). The venv module bootstraps pip via ensurepip.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    gnupg \
    software-properties-common \
    && add-apt-repository -y ppa:deadsnakes/ppa \
    && apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    curl \
    git \
    git-lfs \
    libgl1 \
    libglib2.0-0 \
    libsm6 \
    libxext6 \
    libxrender1 \
    ninja-build \
    pkg-config \
    python3.13 \
    python3.13-dev \
    python3.13-venv \
    && rm -rf /var/lib/apt/lists/*

RUN ln -sf /usr/bin/python3.13 /usr/local/bin/python

# Bake torch (the heaviest, slowest-changing dependency) into its own early
# layer so Docker caches it: it is re-downloaded only when this line changes,
# not every run. Keep TORCH_INDEX_URL in sync with the action's torch_index_url
# input (NVIDIA stable cu130). The runtime venv inherits this via
# system-site-packages and skips re-installing torch.
ARG TORCH_INDEX_URL=https://download.pytorch.org/whl/cu130
RUN python -m ensurepip --upgrade \
    && python -m pip install --no-cache-dir --upgrade pip wheel setuptools \
    && python -m pip install --no-cache-dir torch torchvision torchaudio --extra-index-url ${TORCH_INDEX_URL}

WORKDIR /runner
# Bake the runner and the mock client source so the image is self-contained; the
# action checkout is also mounted at /action at runtime.
COPY scripts/notch_contract_ci.py /runner/notch_contract_ci.py
COPY mock_client /runner/mock_client

ENTRYPOINT ["python", "/runner/notch_contract_ci.py"]
