#!/usr/bin/env python3
"""Docker-side runner for the ComfyUI-Notch contract action."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import shlex
import signal
import subprocess
import sys
import time
import urllib.request
import venv
from pathlib import Path
from typing import Any


# SpoutReceiver is intentionally omitted from the Docker extension initialization check.
# SpoutGL is Windows-only; Linux CI validates the cross-platform node surface.
NOTCH_NODE_CLASSES = ["NotchSingleInput", "NotchOutputNode"]
DEFAULT_COMFY_LISTEN_ADDRESS = "127.0.0.1"
DEFAULT_COMFY_PORT = 8188
DEFAULT_COMFY_SCHEME = "http"


class RunnerError(RuntimeError):
    pass


class CommandRunner:
    def __init__(self, artifacts: Path):
        self.artifacts = artifacts

    def run(
        self,
        command: list[str],
        cwd: Path | None = None,
        log_name: str = "setup.log",
        env: dict[str, str] | None = None,
        check: bool = True,
    ) -> subprocess.CompletedProcess[str]:
        log_path = self.artifacts / log_name
        log_path.parent.mkdir(parents=True, exist_ok=True)
        print(f"+ {' '.join(shlex.quote(part) for part in command)}")
        with log_path.open("a", encoding="utf-8") as log_file:
            log_file.write(f"\n$ {' '.join(shlex.quote(part) for part in command)}\n")
            log_file.flush()
            process = subprocess.Popen(
                command,
                cwd=str(cwd) if cwd else None,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
            captured: list[str] = []
            assert process.stdout is not None
            for line in process.stdout:
                captured.append(line)
                log_file.write(line)
                log_file.flush()
                print(line, end="")
            returncode = process.wait()

        result = subprocess.CompletedProcess(command, returncode, "".join(captured), "")
        if check and returncode != 0:
            raise RunnerError(f"command failed with exit code {returncode}: {' '.join(command)}")
        return result


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def read_url_json(url: str, timeout_seconds: int = 10) -> Any:
    request = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
        return json.loads(response.read().decode("utf-8"))


def poll_url(url: str, timeout_seconds: int) -> None:
    deadline = time.monotonic() + timeout_seconds
    last_error = ""
    while time.monotonic() < deadline:
        try:
            read_url_json(url, timeout_seconds=5)
            return
        except Exception as exc:
            last_error = str(exc)
            time.sleep(1)
    raise RunnerError(f"server did not become reachable at {url}: {last_error}")


def copy_extension(source: Path, destination: Path) -> None:
    if not (source / "nodes.py").is_file():
        raise RunnerError(f"extension source does not look like ComfyUI-Notch: {source}")
    if not (source / "cpp" / "notch_comfy_client" / "CMakeLists.txt").is_file():
        raise RunnerError(f"extension source is missing cpp/notch_comfy_client: {source}")
    if destination.exists():
        shutil.rmtree(destination)

    def ignore(_directory: str, names: list[str]) -> set[str]:
        ignored = {
            ".git",
            ".pytest_cache",
            "__pycache__",
            "build",
            "dist",
            ".mypy_cache",
            ".ruff_cache",
        }
        return {name for name in names if name in ignored}

    shutil.copytree(source, destination, ignore=ignore)


def clone_repo(runner: CommandRunner, repository: str, ref: str, destination: Path, log_name: str) -> None:
    if destination.exists():
        shutil.rmtree(destination)
    if not ref:
        runner.run(["git", "clone", "--depth", "1", repository, str(destination)], log_name=log_name)
        return

    runner.run(["git", "clone", "--filter=blob:none", "--no-checkout", repository, str(destination)], log_name=log_name)
    fetched = runner.run(["git", "fetch", "--depth", "1", "origin", ref], cwd=destination, log_name=log_name, check=False)
    if fetched.returncode == 0:
        runner.run(["git", "checkout", "--detach", "FETCH_HEAD"], cwd=destination, log_name=log_name)
        return
    tag_ref = f"refs/tags/{ref}:refs/tags/{ref}"
    fetched_tag = runner.run(["git", "fetch", "--depth", "1", "origin", tag_ref], cwd=destination, log_name=log_name, check=False)
    if fetched_tag.returncode == 0:
        runner.run(["git", "checkout", "--detach", ref], cwd=destination, log_name=log_name)
        return
    runner.run(["git", "checkout", ref], cwd=destination, log_name=log_name)


def git_commit(runner: CommandRunner, directory: Path) -> str:
    result = runner.run(["git", "rev-parse", "HEAD"], cwd=directory, log_name="git.log", check=False)
    if result.returncode != 0:
        return ""
    return result.stdout.strip().splitlines()[-1]


def create_venv(runner: CommandRunner, workdir: Path) -> tuple[Path, Path]:
    env_dir = workdir / "venv"
    venv.EnvBuilder(with_pip=True, clear=True).create(env_dir)
    bin_dir = env_dir / ("Scripts" if os.name == "nt" else "bin")
    python = bin_dir / ("python.exe" if os.name == "nt" else "python")
    pip = bin_dir / ("pip.exe" if os.name == "nt" else "pip")
    runner.run([str(python), "-m", "pip", "install", "--upgrade", "pip", "wheel", "setuptools"], log_name="python-install.log")
    return python, pip


def install_python_deps(
    runner: CommandRunner,
    python: Path,
    pip: Path,
    comfy_dir: Path,
    extension_dir: Path,
    torch_index_url: str,
    install_torch: bool,
) -> None:
    if install_torch:
        runner.run(
            [
                str(python),
                "-m",
                "pip",
                "install",
                "torch",
                "torchvision",
                "torchaudio",
                "--index-url",
                torch_index_url,
            ],
            log_name="python-install.log",
        )
    comfy_requirements = comfy_dir / "requirements.txt"
    if comfy_requirements.is_file():
        runner.run([str(pip), "install", "-r", str(comfy_requirements)], log_name="python-install.log")
    extension_requirements = extension_dir / "requirements.txt"
    if extension_requirements.is_file():
        runner.run([str(pip), "install", "-r", str(extension_requirements)], log_name="python-install.log")


def compile_cpp_client(runner: CommandRunner, extension_dir: Path, workdir: Path) -> None:
    source_dir = extension_dir / "cpp" / "notch_comfy_client"
    build_dir = workdir / "cpp-build" / "notch_comfy_client"
    runner.run(
        ["cmake", "-S", str(source_dir), "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release", "-G", "Ninja"],
        log_name="cpp-compile.log",
    )
    runner.run(["cmake", "--build", str(build_dir), "--config", "Release"], log_name="cpp-compile.log")
    exe = build_dir / "notch_comfy_client_compile_check"
    if not exe.exists():
        exe = build_dir / "Release" / "notch_comfy_client_compile_check.exe"
    if not exe.exists():
        raise RunnerError("C++ compile check executable was not produced")
    runner.run([str(exe)], log_name="cpp-compile.log")


def python_env_snapshot(runner: CommandRunner, python: Path, artifacts: Path) -> dict[str, Any]:
    freeze = subprocess.run([str(python), "-m", "pip", "freeze"], capture_output=True, text=True, check=False)
    (artifacts / "pip-freeze.txt").write_text(freeze.stdout, encoding="utf-8")
    if freeze.returncode != 0:
        (artifacts / "pip-freeze-error.txt").write_text(freeze.stderr, encoding="utf-8")
        raise RunnerError("pip freeze failed")
    code = (
        "import json, platform, sys\n"
        "data={'python':sys.version,'platform':platform.platform()}\n"
        "try:\n"
        " import torch\n"
        " data['torch']=torch.__version__\n"
        " data['torch_cuda_available']=bool(torch.cuda.is_available())\n"
        " data['torch_cuda_version']=torch.version.cuda\n"
        " data['torch_cuda_device_count']=torch.cuda.device_count() if torch.cuda.is_available() else 0\n"
        "except Exception as e:\n"
        " data['torch_error']=str(e)\n"
        "try:\n"
        " try:\n"
        "  from cuda.bindings import driver as cu\n"
        "  data['cuda_python']='available'\n"
        "  data['cuda_python_driver_api']='cuda.bindings.driver'\n"
        " except ImportError:\n"
        "  from cuda import cuda as cu\n"
        "  data['cuda_python']='available'\n"
        "  data['cuda_python_driver_api']='cuda.cuda'\n"
        "except Exception as e:\n"
        " data['cuda_python_error']=str(e)\n"
        "print(json.dumps(data, sort_keys=True))\n"
    )
    result = runner.run([str(python), "-c", code], log_name="environment.log", check=False)
    try:
        snapshot = json.loads(result.stdout.strip().splitlines()[-1])
    except Exception:
        snapshot = {"python": sys.version, "platform": platform.platform()}
    write_json(artifacts / "python-env.json", snapshot)
    return snapshot


def collect_server_features(base_url: str, artifacts: Path) -> dict[str, Any]:
    try:
        features = read_url_json(f"{base_url}/features", timeout_seconds=20)
    except Exception as exc:
        features = {"unavailable": True, "error": str(exc)}
    write_json(artifacts / "server-feature-flags.json", features if isinstance(features, dict) else {"data": features})
    return features if isinstance(features, dict) else {"data": features}


def command_text(command: list[str]) -> str:
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=False)
    except FileNotFoundError:
        return ""
    return (result.stdout + result.stderr).strip()


def start_comfy_server(
    python: Path,
    comfy_dir: Path,
    host: str,
    port: int,
    flags: str,
    log_path: Path,
) -> subprocess.Popen[Any]:
    command = [
        str(python),
        "main.py",
        f"--listen={host}",
        f"--port={port}",
        *shlex.split(flags or ""),
    ]
    print(f"+ {' '.join(shlex.quote(part) for part in command)}")
    log = log_path.open("w", encoding="utf-8")
    return subprocess.Popen(command, cwd=str(comfy_dir), stdout=log, stderr=subprocess.STDOUT, text=True)


def stop_process(process: subprocess.Popen[Any] | None) -> None:
    if process is None or process.poll() is not None:
        return
    try:
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=15)
    except Exception:
        process.kill()
        process.wait(timeout=15)


def assert_object_info(base_url: str, expected_nodes: list[str], artifacts: Path) -> dict[str, Any]:
    object_info = read_url_json(f"{base_url}/object_info", timeout_seconds=20)
    write_json(artifacts / "object-info-summary.json", {"node_count": len(object_info), "notch_nodes": expected_nodes})
    missing = [node for node in expected_nodes if node not in object_info]
    if missing:
        write_json(artifacts / "object-info-debug.json", {"missing": missing, "keys": sorted(object_info.keys())})
        raise RunnerError(f"/object_info is missing Notch node classes: {missing}")
    return object_info


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run ComfyUI-Notch compatibility checks")
    parser.add_argument(
        "--mode",
        choices=["extension_initialization", "contract_matrix"],
        default="extension_initialization",
    )
    parser.add_argument("--comfyui-repository", required=True)
    parser.add_argument("--comfyui-ref", default="")
    parser.add_argument("--extension-repository", default="")
    parser.add_argument("--extension-ref", default="")
    parser.add_argument("--workspace", default="/workspace")
    parser.add_argument("--workdir", default="/work/notch-contract-ci")
    parser.add_argument("--artifacts", default="/artifacts")
    parser.add_argument("--host", default=DEFAULT_COMFY_LISTEN_ADDRESS)
    parser.add_argument("--port", type=int, default=DEFAULT_COMFY_PORT)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--comfyui-flags", default="--disable-auto-launch")
    parser.add_argument("--torch-index-url", default="https://download.pytorch.org/whl/cu121")
    parser.add_argument("--install-torch", choices=["true", "false"], default="true")
    parser.add_argument("--expected-node-classes", default=",".join(NOTCH_NODE_CLASSES))
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    mode = args.mode
    result_filename = (
        "extension-initialization-result.json"
        if mode == "extension_initialization"
        else "contract-matrix-result.json"
    )
    artifacts = Path(args.artifacts).resolve()
    workdir = Path(args.workdir).resolve()
    workspace = Path(args.workspace).resolve()
    shutil.rmtree(workdir, ignore_errors=True)
    workdir.mkdir(parents=True, exist_ok=True)
    artifacts.mkdir(parents=True, exist_ok=True)

    runner = CommandRunner(artifacts)
    comfy_dir = workdir / "ComfyUI"
    extension_source = workdir / "ComfyUI-Notch-source"
    extension_dir = comfy_dir / "custom_nodes" / "ComfyUI-Notch"
    expected_nodes = [node.strip() for node in args.expected_node_classes.split(",") if node.strip()]
    server: subprocess.Popen[Any] | None = None
    result: dict[str, Any] = {
        "schema_version": 1,
        "profile": mode,
        "result": "fail",
        "checks": {
            "server_started": False,
            "queue_reachable": False,
            "object_info_contains_notch_nodes": False,
            "cpp_client_compiles": False,
        },
    }

    try:
        clone_repo(runner, args.comfyui_repository, args.comfyui_ref, comfy_dir, "git.log")
        if args.extension_repository:
            clone_repo(runner, args.extension_repository, args.extension_ref, extension_source, "git.log")
        else:
            extension_source = workspace
        copy_extension(extension_source, extension_dir)

        python, pip = create_venv(runner, workdir)
        install_python_deps(
            runner,
            python,
            pip,
            comfy_dir,
            extension_dir,
            args.torch_index_url,
            args.install_torch == "true",
        )
        env_snapshot = python_env_snapshot(runner, python, artifacts)

        compile_cpp_client(runner, extension_dir, workdir)
        result["checks"]["cpp_client_compiles"] = True

        base_url = f"{DEFAULT_COMFY_SCHEME}://{args.host}:{args.port}"
        server = start_comfy_server(
            python,
            comfy_dir,
            args.host,
            args.port,
            args.comfyui_flags,
            artifacts / "comfyui.log",
        )
        poll_url(f"{base_url}/queue", args.timeout)
        result["checks"]["server_started"] = True
        result["checks"]["queue_reachable"] = True
        server_features = collect_server_features(base_url, artifacts)
        assert_object_info(base_url, expected_nodes, artifacts)
        result["checks"]["object_info_contains_notch_nodes"] = True

        environment = {
            "schema_version": 1,
            "mode": mode,
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python_env": env_snapshot,
            "nvidia_smi": command_text(["nvidia-smi"]),
            "cmake": command_text(["cmake", "--version"]).splitlines()[:1],
            "git": command_text(["git", "--version"]),
            "comfyui_repository": args.comfyui_repository,
            "comfyui_ref": args.comfyui_ref,
            "comfyui_commit": git_commit(runner, comfy_dir),
            "extension_repository": args.extension_repository or "workspace",
            "extension_ref": args.extension_ref,
            "extension_commit": git_commit(runner, extension_source) if (extension_source / ".git").exists() else "",
            "server_url": base_url,
            "server_feature_flags": server_features.get("extension", {}).get("notch", {}),
            "expected_node_classes": expected_nodes,
        }
        write_json(artifacts / "environment.json", environment)
        result.update(
            {
                "comfyui_ref": args.comfyui_ref,
                "comfyui_commit": environment["comfyui_commit"],
                "comfyui_notch_commit": environment["extension_commit"],
                "runner": {
                    "os": platform.system(),
                    "platform": platform.platform(),
                },
            }
        )

        if mode == "contract_matrix":
            write_json(
                artifacts / result_filename,
                {
                    "schema_version": 1,
                    "result": "not_implemented",
                    "implemented_checks": ["extension_initialization", "cpp_client_compile_check"],
                    "missing_checks": ["live_http_ws_cuda_mock_client_permutation_suite"],
                },
            )
            raise RunnerError("contract_matrix mode needs the CI mock client executable before it can claim a pass")

        result["result"] = "pass"
        write_json(artifacts / result_filename, result)

        compatibility = {
            **result,
            "environment_snapshot_sha256": "",
        }
        write_json(artifacts / "compatibility-result.json", compatibility)
        compatibility["environment_snapshot_sha256"] = file_sha256(artifacts / "environment.json")
        write_json(artifacts / "compatibility-result.json", compatibility)
        return 0
    except Exception as exc:
        result["error"] = str(exc)
        write_json(artifacts / result_filename, result)
        environment_path = artifacts / "environment.json"
        if environment_path.exists():
            try:
                environment_data = json.loads(environment_path.read_text(encoding="utf-8"))
            except Exception:
                environment_data = {}
            environment_data["error"] = str(exc)
            write_json(environment_path, environment_data)
        else:
            write_json(
                environment_path,
                {
                    "schema_version": 1,
                    "mode": mode,
                    "platform": platform.platform(),
                    "machine": platform.machine(),
                    "error": str(exc),
                    "nvidia_smi": command_text(["nvidia-smi"]),
                },
            )
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        stop_process(server)


if __name__ == "__main__":
    raise SystemExit(main())
