# syntax=docker/dockerfile:1.7
ARG UV_VERSION=0.9.9
FROM ghcr.io/astral-sh/uv:${UV_VERSION} AS uv

# CUDA 13 base to match the ComfyUI NVIDIA stable install (torch cu130). Pin the
# exact patch tag to one available on your registry / supported by the runner
# driver; CUDA 13 requires a recent NVIDIA driver on the self-hosted host.
FROM nvidia/cuda:13.0.1-cudnn-devel-ubuntu24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV PYTHONUNBUFFERED=1
ENV UV_CACHE_DIR=/cache/uv
ENV UV_LINK_MODE=copy
# Reuse downloaded wheels across both jobs via the /cache/uv bind mount the
# action provides. If the mount is absent the cache is just container-local.

# Python 3.13 is ComfyUI's recommended interpreter; install it from deadsnakes
# (Ubuntu 24.04 ships 3.12). uv installs runtime deps into this container-owned
# Python environment.
COPY --from=uv /uv /uvx /usr/local/bin/

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
    && rm -rf /var/lib/apt/lists/*

RUN ln -sf /usr/bin/python3.13 /usr/local/bin/python

# Bake torch (the heaviest, slowest-changing dependency) into its own early
# layer so Docker caches it: it is re-downloaded only when this line changes,
# not every run. Keep TORCH_INDEX_URL in sync with the action's torch_index_url
# input (NVIDIA stable cu130). Runtime installs use the same system Python, so
# uv treats baked torch as already satisfied and skips re-installing it.
ARG TORCH_INDEX_URL=https://download.pytorch.org/whl/cu130
RUN --mount=type=cache,target=/cache/uv \
    uv pip install --system torch torchvision torchaudio --index ${TORCH_INDEX_URL}

WORKDIR /runner
# Bake the runner and the mock client source so the image is self-contained; the
# action checkout is also mounted at /action at runtime.
COPY scripts/notch_contract_ci.py /runner/notch_contract_ci.py
COPY mock_client /runner/mock_client

ENTRYPOINT ["python", "/runner/notch_contract_ci.py"]
