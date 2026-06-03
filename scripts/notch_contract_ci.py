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


def emit_annotation(level: str, title: str, message: str) -> None:
    """Emit a GitHub Actions workflow command so it shows on the run overview.

    Stdout from the Docker step is scanned by the runner, so these surface as
    annotations even though the script runs inside the container.
    """

    def esc(value: str) -> str:
        return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")

    title_esc = esc(title).replace(":", "%3A").replace(",", "%2C")
    print(f"::{level} title={title_esc}::{esc(message)}", flush=True)


def annotate_conformance(artifacts: Path, job_label: str) -> None:
    """Surface each conformance case (and the totals) as run-overview annotations.

    job_label titles the totals annotation and should match the workflow job name
    (e.g. "Protocol negotiation", "Local delivery").
    """
    cases_dir = artifacts / "conformance" / "cases"
    if cases_dir.is_dir():
        for case_json in sorted(cases_dir.glob("*/case.json")):
            try:
                rec = json.loads(case_json.read_text(encoding="utf-8"))
            except Exception:
                continue
            result = rec.get("result", "")
            case_id = rec.get("case_id", case_json.parent.name)
            title = rec.get("title", case_id)
            if result in ("fail", "error"):
                errors = ", ".join(rec.get("errors", [])) or result
                emit_annotation("error", case_id, f"{result.upper()} — {title} [{errors}]")
            elif result == "skip":
                emit_annotation("warning", case_id, f"SKIP — {title}")

    totals = {}
    result_path = artifacts / "conformance-result.json"
    if result_path.is_file():
        try:
            totals = json.loads(result_path.read_text(encoding="utf-8")).get("totals", {})
        except Exception:
            totals = {}
    summary = (
        f"pass={totals.get('pass', 0)} fail={totals.get('fail', 0)} "
        f"skip={totals.get('skip', 0)} error={totals.get('error', 0)}"
    )
    emit_annotation("notice", job_label, summary)


def _result_icon(result: str) -> str:
    return {"pass": "✅", "fail": "❌", "error": "🟥", "skip": "⏭️"}.get(result, "•")


def write_conformance_summary(artifacts: Path, job_label: str) -> None:
    """Render a per-job markdown summary (status, totals, per-phase case table) to
    conformance-summary.md, which the launch script appends to $GITHUB_STEP_SUMMARY.

    The HTML report is the deep drill-down; this is the at-a-glance page on every
    conformance job's run page.
    """
    result_path = artifacts / "conformance-result.json"
    if not result_path.is_file():
        return
    try:
        result = json.loads(result_path.read_text(encoding="utf-8"))
    except Exception:
        return
    totals = result.get("totals", {})
    overall = result.get("result", "")
    phase = result.get("phase", "")

    cases_by_phase: dict[str, list[dict[str, Any]]] = {}
    cases_dir = artifacts / "conformance" / "cases"
    if cases_dir.is_dir():
        for case_json in sorted(cases_dir.glob("*/case.json")):
            try:
                rec = json.loads(case_json.read_text(encoding="utf-8"))
            except Exception:
                continue
            cases_by_phase.setdefault(str(rec.get("phase", "other")), []).append(rec)

    banner = "✅ PASS" if overall == "pass" else "❌ FAIL"
    lines = [
        f"## {job_label} — {banner}",
        "",
        f"**pass={totals.get('pass', 0)} · fail={totals.get('fail', 0)} · "
        f"skip={totals.get('skip', 0)} · error={totals.get('error', 0)}**  (phase `{phase}`)",
        "",
    ]
    phase_order = ["liveness", "discovery", "readiness", "transport-selection", "delivery-local", "delivery-remote"]
    ordered = [p for p in phase_order if p in cases_by_phase]
    ordered += [p for p in cases_by_phase if p not in phase_order]
    for current in ordered:
        lines.append(f"### {current}")
        for rec in cases_by_phase[current]:
            icon = _result_icon(str(rec.get("result", "")))
            case_id = str(rec.get("case_id", ""))
            title = str(rec.get("title", case_id))
            errors = ", ".join(rec.get("errors", []) or [])
            suffix = f" — `{errors}`" if errors else ""
            lines.append(f"- {icon} `{case_id}` — {title}{suffix}")
        lines.append("")
    (artifacts / "conformance-summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


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
    # system_site_packages=True so the venv inherits torch baked into the image
    # layer; the runtime torch install is then skipped (see install_python_deps).
    venv.EnvBuilder(with_pip=True, clear=True, system_site_packages=True).create(env_dir)
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
        # Torch is baked into the image layer and visible via system-site-packages,
        # so skip the (multi-GB) install when it already imports. Only install if
        # absent — e.g. a base image without the bake layer.
        already = runner.run(
            [str(python), "-c", "import torch, torchvision, torchaudio"],
            log_name="python-install.log",
            check=False,
        )
        if already.returncode == 0:
            print("torch already present (baked image layer); skipping torch install")
        else:
            runner.run(
                [
                    str(python),
                    "-m",
                    "pip",
                    "install",
                    "torch",
                    "torchvision",
                    "torchaudio",
                    "--extra-index-url",
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


class MockClientBuildError(RunnerError):
    def __init__(self, message: str, code: str):
        super().__init__(message)
        self.code = code


def find_mock_client_dir() -> Path:
    # In the container the runner is baked at /runner with mock_client copied
    # beside it; the action checkout is also mounted at /action. In local dev the
    # source sits next to scripts/. Probe each in order.
    candidates = []
    env_root = os.environ.get("NOTCH_ACTION_ROOT")
    if env_root:
        candidates.append(Path(env_root) / "mock_client")
    script_dir = Path(__file__).resolve().parent
    candidates.append(script_dir / "mock_client")
    candidates.append(script_dir.parent / "mock_client")
    candidates.append(Path("/action") / "mock_client")
    for candidate in candidates:
        if (candidate / "CMakeLists.txt").is_file():
            return candidate
    raise MockClientBuildError(
        "could not locate the mock_client source directory",
        "harness_error",
    )


def build_mock_client(runner: CommandRunner, extension_dir: Path, workdir: Path) -> Path:
    # The mock client links the checked-out client interface, so interface drift
    # surfaces here as a build failure with a stable, contract-framed code.
    source_dir = find_mock_client_dir()
    client_dir = extension_dir / "cpp" / "notch_comfy_client"
    build_dir = workdir / "cpp-build" / "notch_mock_client"
    configure = runner.run(
        [
            "cmake",
            "-S",
            str(source_dir),
            "-B",
            str(build_dir),
            f"-DNOTCH_COMFY_CLIENT_DIR={client_dir}",
            "-DCMAKE_BUILD_TYPE=Release",
            "-G",
            "Ninja",
        ],
        log_name="mock-client-build.log",
        check=False,
    )
    built = configure
    if configure.returncode == 0:
        built = runner.run(
            ["cmake", "--build", str(build_dir), "--config", "Release"],
            log_name="mock-client-build.log",
            check=False,
        )
    if built.returncode != 0:
        raise MockClientBuildError(
            "mock client transport adapters are incompatible with the current notch_comfy_client interface",
            "transport_interface_incompatible",
        )
    stub = build_dir / "notch_mock_stub_check"
    if stub.exists():
        runner.run([str(stub)], log_name="mock-client-build.log")
    exe = build_dir / "notch_mock_client"
    if not exe.exists():
        raise MockClientBuildError(
            "mock client executable was not produced",
            "transport_interface_incompatible",
        )
    return exe


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


def _parse_driver_version(nvidia_smi_text: str) -> str:
    import re

    match = re.search(r"Driver Version:\s*([\d.]+)", nvidia_smi_text)
    return match.group(1) if match else ""


def _render_cuda_summary(diag: dict[str, Any]) -> str:
    """Render the host + container CUDA env and launch-flag assertions as markdown.

    Used both as a foldable job-log group and as a $GITHUB_STEP_SUMMARY section so
    the driver, the container-vs-host IPC/cgroup namespaces, and the flag
    assertions are visible at a glance without digging through the raw log.
    """
    runtime_facts = diag.get("runtime") if isinstance(diag.get("runtime"), dict) else {}
    host = diag.get("host_reference", {}) if isinstance(diag.get("host_reference"), dict) else {}
    side = diag.get("side", "container")
    lines = [
        f"### CUDA environment & launch assertions ({side})",
        "",
    ]
    if diag.get("cuda_applicable") is False:
        lines += [
            "> No GPU visible in this container — CUDA IPC is not used here (e.g. the remote "
            "client is HTTP-only by design, so CUDA is rejected by reachability). The driver "
            "check is not applicable; the namespace/flag checks below still verify the launch config.",
            "",
        ]
    lines += [
        "| fact | value |",
        "| --- | --- |",
        f"| display driver (host-mapped) | `{diag.get('driver_version') or 'unknown'}` |",
        f"| torch / cuda | `{runtime_facts.get('torch', '?')}` / cuda `{runtime_facts.get('torch_cuda_version', '?')}` "
        f"(available={runtime_facts.get('torch_cuda_available', '?')}) |",
        f"| ipc ns — container | `{diag.get('ipc_namespace', '?')}` |",
        f"| ipc ns — host ref | `{host.get('ipc_namespace') or '(not provided)'}` |",
        f"| pid ns — container | `{diag.get('pid_namespace', '?')}` (shared with host when --pid=host) |",
        f"| cgroup ns — container | `{diag.get('cgroup_namespace', '?')}` |",
        f"| cgroup ns — host ref | `{host.get('cgroup_namespace') or '(not provided)'}` |",
        f"| /dev/shm total | `{diag.get('dev_shm_bytes_total', '?')}` bytes |",
        "",
        "| assertion | result | detail |",
        "| --- | --- | --- |",
    ]
    for item in diag.get("assertions", []):
        mark = "✅ pass" if item.get("ok") else "❌ FAIL"
        lines.append(f"| `{item.get('name', '')}` | {mark} | {item.get('detail', '')} |")
    lines.append("")
    return "\n".join(lines)


def collect_cuda_diagnostics(artifacts: Path, python: Path | None = None, side: str = "container") -> dict[str, Any]:
    """Capture CUDA + IPC/cgroup launch facts for CUDA IPC troubleshooting.

    NVIDIA's WSL guidance gives R510 as the broad legacy-CUDA-IPC floor, but it
    does not prove this exact Docker Desktop / WSL2 GPU-PV topology can import a
    memory handle. These diagnostics verify the runner did not silently drop the
    namespace flags we intentionally pass, then the CUDA IPC probe records the
    raw import behavior as evidence.

    The launch script passes the host PID-1 namespace inodes (HOST_IPC_NS /
    HOST_CGROUP_NS) from a --pid=host probe; when present the checks are exact —
    the container namespace must equal the host namespace. cgroupns is
    additionally self-detected from /proc/self/cgroup ('0::/' means a private
    cgroup namespace, i.e. --cgroupns=host did NOT take effect).
    """
    diag: dict[str, Any] = {"schema_version": 1, "side": side}
    nvidia_smi = command_text(["nvidia-smi"])
    diag["nvidia_smi"] = nvidia_smi
    diag["driver_version"] = _parse_driver_version(nvidia_smi)
    diag["libcuda_ldconfig"] = command_text(
        ["bash", "-lc", "ldconfig -p | grep -E 'libcuda\\.so|libcudart\\.so' || true"]
    )
    diag["nvidia_container_cli"] = command_text(["nvidia-container-cli", "-V"])
    diag["wsl_version"] = command_text(["wsl.exe", "--version"]) or command_text(["wsl", "--version"])
    (artifacts / "libcuda-ldconfig.txt").write_text(diag["libcuda_ldconfig"] + "\n", encoding="utf-8")
    (artifacts / "nvidia-container-cli.txt").write_text(diag["nvidia_container_cli"] + "\n", encoding="utf-8")
    (artifacts / "wsl-version.txt").write_text(diag["wsl_version"] + "\n", encoding="utf-8")

    def _readlink(path: str) -> str:
        try:
            return os.readlink(path)
        except OSError as exc:
            return f"error: {exc}"

    diag["ipc_namespace"] = _readlink("/proc/self/ns/ipc")
    diag["pid_namespace"] = _readlink("/proc/self/ns/pid")
    diag["cgroup_namespace"] = _readlink("/proc/self/ns/cgroup")
    try:
        diag["cgroup_path"] = Path("/proc/self/cgroup").read_text(encoding="utf-8").strip()
    except OSError as exc:
        diag["cgroup_path"] = f"error: {exc}"
    try:
        stat = os.statvfs("/dev/shm")
        diag["dev_shm_bytes_total"] = stat.f_blocks * stat.f_frsize
        diag["dev_shm_bytes_free"] = stat.f_bavail * stat.f_frsize
    except OSError as exc:
        diag["dev_shm_error"] = str(exc)

    host_ipc_ns = os.environ.get("HOST_IPC_NS", "").strip()
    host_cgroup_ns = os.environ.get("HOST_CGROUP_NS", "").strip()
    expect_ipc_host = os.environ.get("EXPECT_IPC_HOST") == "1"
    expect_cgroupns_host = os.environ.get("EXPECT_CGROUPNS_HOST") == "1"
    diag["host_reference"] = {"ipc_namespace": host_ipc_ns, "cgroup_namespace": host_cgroup_ns}
    diag["expectations"] = {"ipc_host": expect_ipc_host, "cgroupns_host": expect_cgroupns_host}

    assertions: list[dict[str, Any]] = []

    driver = diag["driver_version"]
    try:
        driver_major = int(driver.split(".")[0]) if driver else 0
    except ValueError:
        driver_major = 0
    # A container with no visible GPU (e.g. the remote client, which only needs
    # HTTP and never does CUDA IPC) reports no driver. The driver-support check
    # only applies where CUDA IPC is actually used, so gate it on a GPU being
    # present; otherwise it is not-applicable, not a failure.
    diag["cuda_applicable"] = driver_major > 0
    if driver_major > 0:
        assertions.append(
            {
                "name": "driver_supports_legacy_cuda_ipc",
                "ok": driver_major >= 510,
                "detail": f"host display driver {driver} (WSL legacy CUDA IPC documented floor is R510; not a topology guarantee)",
            }
        )

    if expect_ipc_host:
        if host_ipc_ns:
            assertions.append(
                {
                    "name": "ipc_namespace_matches_host",
                    "ok": diag["ipc_namespace"] == host_ipc_ns,
                    "detail": f"container {diag['ipc_namespace']} vs host {host_ipc_ns} (--ipc=host)",
                }
            )
        else:
            shm_total = diag.get("dev_shm_bytes_total", 0)
            assertions.append(
                {
                    "name": "ipc_namespace_likely_host",
                    "ok": isinstance(shm_total, int) and shm_total != 67108864,
                    "detail": f"/dev/shm total {shm_total} bytes (heuristic; default private shm is 64MiB)",
                    "heuristic": True,
                }
            )

    if expect_cgroupns_host:
        cgroup_path = diag.get("cgroup_path", "")
        self_detect_host = isinstance(cgroup_path, str) and cgroup_path.strip() not in ("0::/", "")
        if host_cgroup_ns:
            assertions.append(
                {
                    "name": "cgroup_namespace_matches_host",
                    "ok": diag["cgroup_namespace"] == host_cgroup_ns,
                    "detail": f"container {diag['cgroup_namespace']} vs host {host_cgroup_ns} (--cgroupns=host)",
                }
            )
        assertions.append(
            {
                "name": "cgroup_namespace_is_host_view",
                "ok": self_detect_host,
                "detail": f"/proc/self/cgroup = '{cgroup_path}' (a private cgroup ns reports '0::/')",
            }
        )

    if python is not None:
        code = (
            "import json\n"
            "d={}\n"
            "try:\n"
            " import torch\n"
            " d['torch']=torch.__version__\n"
            " d['torch_cuda_version']=torch.version.cuda\n"
            " d['torch_cuda_available']=bool(torch.cuda.is_available())\n"
            "except Exception as e:\n"
            " d['torch_error']=str(e)\n"
            "print(json.dumps(d, sort_keys=True))\n"
        )
        result = subprocess.run([str(python), "-c", code], capture_output=True, text=True, check=False)
        try:
            diag["runtime"] = json.loads(result.stdout.strip().splitlines()[-1])
        except Exception:
            diag["runtime_error"] = result.stderr.strip()[:500]

    diag["assertions"] = assertions
    diag["assertions_ok"] = all(item["ok"] for item in assertions)
    write_json(artifacts / "cuda-diagnostics.json", diag)

    # Write a markdown section (picked up into $GITHUB_STEP_SUMMARY host-side) and
    # emit the same content as a foldable group in this step's log.
    summary_md = _render_cuda_summary(diag)
    (artifacts / "cuda-summary.md").write_text(summary_md + "\n", encoding="utf-8")
    print(f"::group::CUDA environment & launch assertions ({side})", flush=True)
    print(summary_md, flush=True)
    print("::endgroup::", flush=True)

    for item in assertions:
        if not item["ok"]:
            emit_annotation("warning", f"CUDA launch ({side})", f"{item['name']}: {item['detail']}")
    return diag


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


def collect_official_simple_ipc_sample(runner: CommandRunner, artifacts: Path) -> dict[str, Any]:
    """Run NVIDIA's simpleIPC sample when the CUDA samples are present.

    The CUDA devel images usually do not include sample source, so this is an
    opportunistic datapoint. Absence is recorded as a skip artifact, not a
    failure.
    """

    script = r"""
set -u
sample_dir=""
for candidate in \
  /usr/local/cuda/samples/0_Simple/simpleIPC \
  /usr/local/cuda/samples/Samples/0_Introduction/simpleIPC \
  /usr/local/cuda-*/samples/0_Simple/simpleIPC \
  /usr/local/cuda-*/samples/Samples/0_Introduction/simpleIPC
do
  if [ -d "$candidate" ]; then
    sample_dir="$candidate"
    break
  fi
done
if [ -z "$sample_dir" ]; then
  echo "simpleIPC sample source not found under /usr/local/cuda*/samples"
  exit 0
fi
echo "simpleIPC directory: $sample_dir"
cd "$sample_dir" || exit 2
if [ -f Makefile ]; then
  make clean || true
  make || exit $?
fi
if [ ! -x ./simpleIPC ]; then
  echo "./simpleIPC not found or not executable"
  exit 2
fi
./simpleIPC
"""
    run = runner.run(["bash", "-lc", script], log_name="cuda-simple-ipc.log", check=False)
    available = "simpleIPC sample source not found" not in run.stdout
    result = {
        "schema_version": 1,
        "probe": "nvidia_simpleIPC",
        "available": available,
        "returncode": run.returncode,
        "log": "cuda-simple-ipc.log",
        "note": "sample is run as ./simpleIPC when source is available",
    }
    if not available:
        result["result"] = "skip"
        result["reason"] = "sample_source_not_found"
    else:
        result["result"] = "pass" if run.returncode == 0 else "fail"
    write_json(artifacts / "cuda-simple-ipc-result.json", result)
    if available and run.returncode != 0:
        emit_annotation("warning", "CUDA simpleIPC", "NVIDIA simpleIPC failed; see cuda-simple-ipc.log")
    return result


def collect_cuda_ipc_probe(runner: CommandRunner, mock_exe: Path, artifacts: Path) -> dict[str, Any]:
    """Run the pure C++ same-container CUDA IPC repro built with the mock client.

    The probe is diagnostic evidence only. The delivery matrix decides whether
    the actual Notch CUDA round trip passes, fails, or is skipped as a platform
    limitation.
    """

    exe_name = "notch_cuda_ipc_probe.exe" if os.name == "nt" else "notch_cuda_ipc_probe"
    probe = mock_exe.parent / exe_name
    if not probe.exists():
        result = {
            "schema_version": 1,
            "probe": "notch_cuda_ipc_probe",
            "available": False,
            "result": "skip",
            "reason": "probe_not_built",
            "detail": "CUDAToolkit was not found by CMake, or this platform is not supported by the probe",
        }
        write_json(artifacts / "cuda-ipc-probe.json", result)
        return result

    ld_debug_command = (
        "LD_DEBUG=libs "
        + shlex.quote(str(probe))
        + " 2>&1 | grep -E 'libcuda|libcudart' | head -80 || true"
    )
    runner.run(["bash", "-lc", ld_debug_command], log_name="cuda-ipc-ld-debug.log", check=False)

    run = runner.run([str(probe)], log_name="cuda-ipc-probe.log", check=False)
    parsed: dict[str, Any]
    try:
        parsed = json.loads(run.stdout.strip().splitlines()[-1])
    except Exception:
        parsed = {
            "schema_version": 1,
            "probe": "notch_cuda_ipc_probe",
            "result": "error",
            "parse_error": "probe did not print JSON on its final stdout line",
        }
    parsed["available"] = True
    parsed["returncode"] = run.returncode
    parsed["log"] = "cuda-ipc-probe.log"
    parsed["ld_debug_log"] = "cuda-ipc-ld-debug.log"
    write_json(artifacts / "cuda-ipc-probe.json", parsed)
    if parsed.get("result") == "fail":
        emit_annotation(
            "warning",
            "CUDA IPC probe",
            "Pure C++ same-container CUDA IPC repro failed; see cuda-ipc-probe.json",
        )
    elif parsed.get("result") == "error":
        emit_annotation("warning", "CUDA IPC probe", "Probe did not complete cleanly; see cuda-ipc-probe.log")
    return parsed


def collect_cuda_ipc_evidence(runner: CommandRunner, mock_exe: Path, artifacts: Path) -> None:
    collect_official_simple_ipc_sample(runner, artifacts)
    collect_cuda_ipc_probe(runner, mock_exe, artifacts)


def start_comfy_server(
    python: Path,
    comfy_dir: Path,
    host: str,
    port: int,
    flags: str,
    log_path: Path,
    extra_env: dict[str, str] | None = None,
) -> subprocess.Popen[Any]:
    command = [
        str(python),
        "main.py",
        f"--listen={host}",
        f"--port={port}",
        *shlex.split(flags or ""),
    ]
    print(f"+ {' '.join(shlex.quote(part) for part in command)}")
    env = {**os.environ, **(extra_env or {})}
    log = log_path.open("w", encoding="utf-8")
    return subprocess.Popen(
        command, cwd=str(comfy_dir), env=env, stdout=log, stderr=subprocess.STDOUT, text=True
    )


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
        choices=[
            "extension_boot",
            "protocol_negotiation",
            "delivery_local",
            "delivery_remote_server",
            "delivery_remote_client",
        ],
        default="extension_boot",
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
    parser.add_argument("--torch-index-url", default="https://download.pytorch.org/whl/cu130")
    parser.add_argument("--install-torch", choices=["true", "false"], default="true")
    parser.add_argument("--expected-node-classes", default=",".join(NOTCH_NODE_CLASSES))
    parser.add_argument("--named-route-id", default="")
    parser.add_argument("--named-route-server-root", default="")
    parser.add_argument("--named-route-client-root", default="")
    parser.add_argument("--named-route-relative-directory", default="remote-route")
    return parser.parse_args()


CONFORMANCE_MODES = {
    "protocol_negotiation": "negotiation",
    "delivery_local": "delivery-local",
    "delivery_remote_client": "delivery-remote",
}

# Human job labels (match the workflow job names) used for annotation titles.
JOB_LABELS = {
    "extension_boot": "Extension boot",
    "protocol_negotiation": "Protocol negotiation",
    "delivery_local": "Local delivery",
    "delivery_remote_client": "Remote delivery",
}


def main() -> int:
    args = parse_args()
    mode = args.mode
    conformance_phase = CONFORMANCE_MODES.get(mode)
    result_filename = (
        "extension-boot-result.json"
        if mode == "extension_boot"
        else "conformance-result.json"
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
        if mode == "delivery_remote_client":
            if not (workspace / "cpp" / "notch_comfy_client" / "CMakeLists.txt").is_file():
                raise RunnerError(f"workspace is missing cpp/notch_comfy_client: {workspace}")

            base_url = f"{DEFAULT_COMFY_SCHEME}://{args.host}:{args.port}"
            poll_url(f"{base_url}/queue", args.timeout)
            remote_features = collect_server_features(base_url, artifacts)
            collect_cuda_diagnostics(artifacts, None, side=mode)

            try:
                mock_exe = build_mock_client(runner, workspace, workdir)
            except MockClientBuildError as exc:
                result["result"] = "fail"
                result["setup_failure_code"] = exc.code
                result["error"] = str(exc)
                write_json(
                    artifacts / "transport-interface-result.json",
                    {
                        "schema_version": 1,
                        "result": "fail",
                        "setup_failure_code": exc.code,
                        "error": str(exc),
                        "log": "mock-client-build.log",
                    },
                )
                write_json(artifacts / result_filename, result)
                emit_annotation("error", "Mock client build", f"{exc.code}: {exc}")
                return 1

            environment = {
                "schema_version": 1,
                "mode": mode,
                "platform": platform.platform(),
                "machine": platform.machine(),
                "extension_repository": args.extension_repository or "workspace",
                "extension_ref": args.extension_ref,
                "extension_commit": git_commit(runner, workspace) if (workspace / ".git").exists() else "",
                "server_url": base_url,
                "server_feature_flags": remote_features.get("extension", {}).get("notch", {}),
                "named_route": {
                    "named_route_id": args.named_route_id,
                    "client_root": args.named_route_client_root,
                    "relative_directory": args.named_route_relative_directory,
                },
            }
            write_json(artifacts / "environment.json", environment)

            mock_command = [
                str(mock_exe),
                "--base-url",
                base_url,
                "--output-dir",
                str(artifacts),
                "--client-id",
                "notch-conformance-remote",
                "--phase",
                "delivery-remote",
                "--asset-root",
                str(workspace / "tests" / "assets" / "round_trip"),
                "--server-log",
                str(artifacts / "server" / "comfyui.log"),
            ]
            if args.named_route_id and args.named_route_client_root:
                mock_command += [
                    "--named-route-id",
                    args.named_route_id,
                    "--named-route-client-root",
                    args.named_route_client_root,
                    "--named-route-relative-directory",
                    args.named_route_relative_directory,
                ]
            run = runner.run(mock_command, log_name="mock-client.log", check=False)
            conformance_result: dict[str, Any] = {}
            conformance_path = artifacts / result_filename
            if conformance_path.is_file():
                try:
                    conformance_result = json.loads(conformance_path.read_text(encoding="utf-8"))
                except Exception:
                    conformance_result = {}
            passed = run.returncode == 0 and conformance_result.get("result") == "pass"
            compatibility = {
                "schema_version": 1,
                "profile": mode,
                "phase": "delivery-remote",
                "result": "pass" if passed else "fail",
                "comfyui_ref": args.comfyui_ref,
                "comfyui_commit": "",
                "comfyui_notch_commit": environment["extension_commit"],
                "runner": {"os": platform.system(), "platform": platform.platform()},
                "checks": {"remote_server_reachable": True, "mock_client_built": True},
                "conformance": conformance_result.get("totals", {}),
                "environment_snapshot_sha256": file_sha256(artifacts / "environment.json"),
            }
            write_json(artifacts / "compatibility-result.json", compatibility)
            annotate_conformance(artifacts, JOB_LABELS.get(mode, "Remote delivery"))
            write_conformance_summary(artifacts, JOB_LABELS.get(mode, "Remote delivery"))
            return 0 if passed else 1

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
        collect_cuda_diagnostics(artifacts, python, side=mode)

        compile_cpp_client(runner, extension_dir, workdir)
        result["checks"]["cpp_client_compiles"] = True

        connect_host = "127.0.0.1" if args.host in ("0.0.0.0", "::") else args.host
        base_url = f"{DEFAULT_COMFY_SCHEME}://{connect_host}:{args.port}"
        # Run the server with NOTCH_CI so it captures prompt-scoped WARNING+ logs
        # and explicit CI decision events for GET /notch/diagnostics. Mirror the
        # same records to a JSONL artifact; the matrix collects them per case.
        server_extra_env = {
            "NOTCH_CI": "1",
            "NOTCH_CI_LOG": str(artifacts / "notch-ci.jsonl"),
        }
        if args.named_route_id and args.named_route_server_root:
            server_extra_env["NOTCH_NAMED_ROUTES"] = json.dumps(
                {
                    "routes": [
                        {
                            "named_route_id": args.named_route_id,
                            "server_root": args.named_route_server_root,
                        }
                    ]
                }
            )

        server = start_comfy_server(
            python,
            comfy_dir,
            args.host,
            args.port,
            args.comfyui_flags,
            artifacts / "comfyui.log",
            extra_env=server_extra_env,
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
            "named_route": {
                "named_route_id": args.named_route_id,
                "server_root": args.named_route_server_root,
                "relative_directory": args.named_route_relative_directory,
            },
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

        if mode == "delivery_remote_server":
            result["result"] = "pass"
            write_json(artifacts / "extension-boot-result.json", result)
            emit_annotation(
                "notice",
                "Remote delivery server",
                "ComfyUI started and is waiting for the remote mock client",
            )
            while True:
                time.sleep(1)

        if conformance_phase is not None:
            try:
                mock_exe = build_mock_client(runner, extension_dir, workdir)
            except MockClientBuildError as exc:
                result["result"] = "fail"
                result["setup_failure_code"] = exc.code
                result["error"] = str(exc)
                write_json(
                    artifacts / "transport-interface-result.json",
                    {
                        "schema_version": 1,
                        "result": "fail",
                        "setup_failure_code": exc.code,
                        "error": str(exc),
                        "log": "mock-client-build.log",
                    },
                )
                write_json(artifacts / result_filename, result)
                emit_annotation("error", "Mock client build", f"{exc.code}: {exc}")
                return 1

            if mode == "delivery_local":
                collect_cuda_ipc_evidence(runner, mock_exe, artifacts)

            fixtures = find_mock_client_dir() / "fixtures"
            mock_command = [
                str(mock_exe),
                "--base-url",
                base_url,
                "--output-dir",
                str(artifacts),
                "--client-id",
                "notch-conformance",
                "--phase",
                conformance_phase,
            ]
            # Negotiation fixtures exercise the type-axis and readiness-decision
            # cases. Missing fixtures are recorded as skips, not failures.
            fixture_flags: dict[str, Path] = {}
            if conformance_phase == "negotiation":
                fixture_flags = {
                    "--parse-workflow": fixtures / "parse_workflow.json",
                    "--required-files-ready": fixtures / "required_files_ready.json",
                    "--required-files-missing": fixtures / "required_files_missing.json",
                }
            for flag, path in fixture_flags.items():
                if path.is_file():
                    mock_command += [flag, str(path)]
            if conformance_phase == "delivery-local":
                mock_command += [
                    "--asset-root",
                    str(extension_source / "tests" / "assets" / "round_trip"),
                    "--server-log",
                    str(artifacts / "comfyui.log"),
                    "--local-output-path",
                    "/tmp/notch-conformance-output",
                ]
            run = runner.run(mock_command, log_name="mock-client.log", check=False)
            # The mock client owns conformance-result.json; read it back rather
            # than overwriting its per-case totals.
            conformance_result: dict[str, Any] = {}
            conformance_path = artifacts / result_filename
            if conformance_path.is_file():
                try:
                    conformance_result = json.loads(conformance_path.read_text(encoding="utf-8"))
                except Exception:
                    conformance_result = {}
            passed = run.returncode == 0 and conformance_result.get("result") == "pass"
            compatibility = {
                "schema_version": 1,
                "profile": mode,
                "phase": conformance_phase,
                "result": "pass" if passed else "fail",
                "comfyui_ref": args.comfyui_ref,
                "comfyui_commit": environment["comfyui_commit"],
                "comfyui_notch_commit": environment["extension_commit"],
                "runner": result["runner"],
                "checks": {**result["checks"], "mock_client_built": True},
                "conformance": conformance_result.get("totals", {}),
                "environment_snapshot_sha256": file_sha256(artifacts / "environment.json"),
            }
            write_json(artifacts / "compatibility-result.json", compatibility)
            annotate_conformance(artifacts, JOB_LABELS.get(mode, conformance_phase))
            write_conformance_summary(artifacts, JOB_LABELS.get(mode, conformance_phase))
            return 0 if passed else 1

        result["result"] = "pass"
        write_json(artifacts / result_filename, result)
        emit_annotation(
            "notice",
            "Extension boot",
            "ComfyUI started, Notch nodes loaded, client interface compiled",
        )

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
