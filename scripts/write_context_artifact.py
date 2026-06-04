"""Write report-consumable artifacts from ComfyUI-Notch CI context."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def write_metadata_artifact(output: Path, metadata_path: Path, *, result: str, errors: list[str]) -> None:
    metadata = _read_json(metadata_path)
    environment = _metadata_environment(metadata)
    case = {
        "case_id": "version-metadata",
        "title": "Version and compatibility metadata",
        "phase": "metadata",
        "description": (
            "Validates pyproject.toml as the human-edited authority; generated Python, C++, "
            "current manifest, and release manifest artifacts are current. Plugin/server, "
            "C++ client, and ComfyUI compatibility facts are kept distinct."
        ),
        "spec_ref": "VERSIONING.md",
        "result": result,
        "errors": errors,
        "expected": {
            "source_of_truth": "pyproject.toml",
            "generated_artifacts": [
                "core/_generated_versions.py",
                "cpp/notch_comfy_client/include/notch_comfy_client/version.hpp",
                "compatibility/current.json",
                "compatibility/releases.json",
            ],
            "runtime_compatibility_key": "C++ client supports_protocol intersects plugin/server supports_protocol",
        },
        "actual": {
            **_metadata_actual(metadata),
            "metadata_checks": {
                "result": result,
                "errors": errors,
            },
        },
    }
    _write_job_artifact(output, profile="metadata", label_case=case, environment=environment, result=result)


def write_docker_preflight_artifact(output: Path, metadata_path: Path, *, log_path: Path | None) -> None:
    metadata = _read_json(metadata_path)
    environment = _metadata_environment(metadata)
    error_text = _read_text(log_path) if log_path is not None else ""
    case = {
        "case_id": "docker-preflight",
        "title": "Docker runner availability",
        "phase": "extension_boot",
        "description": (
            "Checks that the self-hosted Windows runner can reach Docker before the live "
            "ComfyUI extension boot action starts."
        ),
        "spec_ref": ".github/workflows/notch-comfy-conformance.yml",
        "result": "fail",
        "errors": ["docker_unavailable"],
        "expected": {"docker": "daemon reachable from the self-hosted runner"},
        "actual": {"docker": "unavailable", "log": error_text.strip()},
    }
    _write_job_artifact(output, profile="extension_boot", label_case=case, environment=environment, result="fail")
    if error_text:
        (output / "conformance" / "cases" / "docker-preflight" / "server.log").write_text(error_text, encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    metadata = subparsers.add_parser("metadata")
    metadata.add_argument("--metadata", type=Path, required=True)
    metadata.add_argument("--output", type=Path, required=True)
    metadata.add_argument("--result", choices=["pass", "fail"], default="pass")
    metadata.add_argument("--error", action="append", default=[])

    docker = subparsers.add_parser("docker-preflight")
    docker.add_argument("--metadata", type=Path, required=True)
    docker.add_argument("--output", type=Path, required=True)
    docker.add_argument("--log", type=Path)

    args = parser.parse_args(argv)
    if args.command == "metadata":
        write_metadata_artifact(args.output, args.metadata, result=args.result, errors=args.error)
    elif args.command == "docker-preflight":
        write_docker_preflight_artifact(args.output, args.metadata, log_path=args.log)
    else:  # pragma: no cover - argparse prevents this.
        parser.error(f"unknown command: {args.command}")
    return 0


def _metadata_environment(metadata: dict[str, Any]) -> dict[str, Any]:
    environment = metadata.get("environment")
    if isinstance(environment, dict):
        return environment
    return {}


def _metadata_actual(metadata: dict[str, Any]) -> dict[str, Any]:
    actual = metadata.get("actual")
    if isinstance(actual, dict):
        return actual
    return {}


def _write_job_artifact(
    output: Path,
    *,
    profile: str,
    label_case: dict[str, Any],
    environment: dict[str, Any],
    result: str,
) -> None:
    totals = {"pass": 1 if result == "pass" else 0, "fail": 1 if result == "fail" else 0, "skip": 0, "error": 0}
    output.mkdir(parents=True, exist_ok=True)
    (output / "conformance" / "cases" / label_case["case_id"]).mkdir(parents=True, exist_ok=True)
    _write_json(output / "conformance-result.json", {"result": result, "totals": totals})
    _write_json(output / "compatibility-result.json", {"profile": profile, "result": result, "conformance": totals})
    _write_json(output / "environment.json", environment)
    _write_json(output / "conformance" / "cases" / label_case["case_id"] / "case.json", label_case)


def _read_json(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"expected object in {path}")
    return data


def _write_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _read_text(path: Path | None) -> str:
    if path is None or not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


if __name__ == "__main__":
    raise SystemExit(main())
