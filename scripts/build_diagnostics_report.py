#!/usr/bin/env python3
"""Build one self-contained diagnostics-report.html from extracted CI artifacts.

The report aggregates every conformance job's artifact folder into a single
portable HTML file: all data is inlined (no sibling files, no server, no network
at view time), so a developer can download the one file and open it via file://.

Stdlib-only on purpose — it runs in the report aggregation job with no extra
toolchain. The dynamic payload is one normalized JSON blob inlined into a static
template; the security-critical part is escaping that JSON so it cannot break out
of its <script> element (see json_for_script).
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
from pathlib import Path
from string import Template

# Canonical pipeline order + human labels (match the workflow job names).
JOB_LABELS = {
    "unit_tests": "Unit tests",
    "extension_boot": "Extension boot",
    "protocol_negotiation": "Protocol negotiation",
    "delivery_local": "Local delivery",
    "delivery_remote": "Remote delivery",
}
JOB_ORDER = list(JOB_LABELS)


# --- safe inlining -----------------------------------------------------------


def json_for_script(payload: object) -> str:
    """Serialize payload for safe embedding in a <script type="application/json"> block.

    Escapes the sequences that could break out of the script element or corrupt
    the inlined JSON when the browser reads textContent:
      <  -> \\u003c   (neutralizes </script>, <!--, <script — every '<')
      U+2028 / U+2029 -> \\u2028 / \\u2029 (valid in JSON, illegal in JS string literals)
    Still valid JSON: replacing \\u003c back to '<' yields the original document,
    so JSON.parse(textContent) recovers payload.
    """
    text = json.dumps(payload, ensure_ascii=False, sort_keys=True)
    text = text.replace("<", "\\u003c")
    # U+2028 / U+2029 are valid in JSON but illegal in JS string literals; escape
    # them by codepoint so the source cannot ambiguously normalize them to spaces.
    text = text.replace(chr(0x2028), "\\u2028")
    text = text.replace(chr(0x2029), "\\u2029")
    return text


def truncate_log(text: str, head: int = 200, tail: int = 200) -> dict:
    """Split a log into head/tail previews so very large logs stay openable.

    Returns {head, tail, truncated, total_lines}. When the log fits within
    head+tail lines it is returned whole in `head` with `tail` empty.
    """
    lines = text.splitlines()
    total = len(lines)
    if total <= head + tail:
        return {"head": lines, "tail": [], "truncated": False, "total_lines": total}
    return {"head": lines[:head], "tail": lines[-tail:], "truncated": True, "total_lines": total}


# --- artifact reading --------------------------------------------------------


def _read_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return ""


def _normalize_cases(job_dir: Path) -> list[dict]:
    cases: list[dict] = []
    cases_dir = job_dir / "conformance" / "cases"
    if not cases_dir.is_dir():
        return cases
    for case_json in sorted(cases_dir.glob("*/case.json")):
        rec = _read_json(case_json)
        case_dir = case_json.parent
        if not isinstance(rec, dict):
            cases.append(
                {
                    "id": case_dir.name,
                    "title": case_dir.name,
                    "phase": "other",
                    "result": "error",
                    "errors": ["case_json_unreadable"],
                }
            )
            continue
        evidence: dict = {}
        for key, fname in (
            ("hashes", "hashes.json"),
            ("websocket_summary", "websocket-summary.json"),
            ("negotiation", "negotiation.json"),
            ("notch_diagnostics", "notch-diagnostics.json"),
        ):
            value = _read_json(case_dir / fname)
            if value is not None:
                evidence[key] = value
        server_log = case_dir / "server.log"
        if server_log.is_file():
            evidence["server_log"] = truncate_log(_read_text(server_log), head=80, tail=80)
        cases.append(
            {
                "id": rec.get("case_id", case_dir.name),
                "title": rec.get("title", ""),
                "phase": rec.get("phase", "other"),
                "description": rec.get("description", ""),
                "spec_ref": rec.get("spec_ref", ""),
                "result": rec.get("result", ""),
                "errors": rec.get("errors", []),
                "expected": rec.get("expected"),
                "actual": rec.get("actual"),
                "evidence": evidence,
            }
        )
    return cases


def _normalize_diagnostics(job_dir: Path) -> list[dict]:
    diagnostics: list[dict] = []
    ci_log = job_dir / "notch-ci.jsonl"
    if not ci_log.is_file():
        return diagnostics
    for line in _read_text(ci_log).splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except Exception:
            continue
        if isinstance(rec, dict) and (rec.get("severity") or rec.get("kind")):
            diagnostics.append(
                {
                    "severity": str(rec.get("severity", "")),
                    "kind": str(rec.get("kind", "")),
                    "name": str(rec.get("name") or rec.get("event") or ""),
                    "message": str(rec.get("message", "")),
                    "prompt_id": str(rec.get("prompt_id", "")),
                }
            )
    return diagnostics


def normalize_job(name: str, label: str, job_dir: Path) -> dict:
    """Walk one job's artifact folder into the normalized job shape."""
    job_dir = Path(job_dir)
    result = _read_json(job_dir / "conformance-result.json") or {}
    boot = _read_json(job_dir / "extension-boot-result.json")
    env = _read_json(job_dir / "environment.json") or {}
    py = _read_json(job_dir / "python-env.json") or {}

    logs: dict = {}
    for key, fname in (
        ("comfyui", "comfyui.log"),
        ("mock_client", "mock-client.log"),
        ("mock_client_build", "mock-client-build.log"),
    ):
        log_file = job_dir / fname
        if log_file.is_file():
            logs[key] = truncate_log(_read_text(log_file), head=120, tail=120)

    nvidia = (env.get("nvidia_smi", "") or "").splitlines()
    environment = {
        "platform": env.get("platform", ""),
        "gpu": nvidia[3].strip() if len(nvidia) > 3 else (nvidia[0] if nvidia else ""),
        "torch": py.get("torch", ""),
        "cuda": py.get("torch_cuda_version", ""),
        "comfyui_ref": env.get("comfyui_ref", ""),
        "comfyui_commit": env.get("comfyui_commit", ""),
        "extension_commit": env.get("extension_commit", ""),
        "feature_flags": env.get("server_feature_flags", {}),
    }

    totals = result.get("totals", {}) if isinstance(result, dict) else {}
    result_str = (result.get("result") if isinstance(result, dict) else "") or (
        boot.get("result") if isinstance(boot, dict) else ""
    ) or ""

    return {
        "name": name,
        "label": label,
        "result": result_str,
        "totals": totals,
        "cases": _normalize_cases(job_dir),
        "environment": environment,
        "cuda": _read_json(job_dir / "cuda-diagnostics.json"),
        "server_cuda": _read_json(job_dir / "server" / "cuda-diagnostics.json"),
        "diagnostics": _normalize_diagnostics(job_dir),
        "logs": logs,
        "boot": boot if isinstance(boot, dict) else None,
    }


def aggregate_run(jobs: dict, run_meta: dict | None = None) -> dict:
    """Combine per-job (label, dir) entries into one normalized run dict."""
    normalized = [normalize_job(name, label, Path(job_dir)) for name, (label, job_dir) in jobs.items()]
    order = {name: index for index, name in enumerate(JOB_ORDER)}
    normalized.sort(key=lambda job: order.get(job["name"], 99))

    summary = {"jobs": len(normalized), "pass": 0, "fail": 0, "skip": 0, "error": 0}
    for job in normalized:
        for key in ("pass", "fail", "skip", "error"):
            summary[key] += int(job["totals"].get(key, 0) or 0)
    bad = any(
        (job["totals"].get("fail", 0) or 0) or (job["totals"].get("error", 0) or 0) or job["result"] == "fail"
        for job in normalized
    )
    summary["result"] = "fail" if bad else "pass"

    return {
        "generated_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "run": run_meta or {},
        "summary": summary,
        "jobs": normalized,
    }


def render_report(run: dict, template_path: Path) -> str:
    """Render the static template with the run inlined as escaped JSON."""
    template = Template(Path(template_path).read_text(encoding="utf-8"))
    return template.safe_substitute(
        run_title=str(run.get("run", {}).get("id", "")),
        generated_at=str(run.get("generated_at", "")),
        data_json=json_for_script(run),
    )


def discover_jobs(artifacts_root: Path) -> dict:
    """Map each downloaded artifact folder to a known job by its mode in the name.

    download-artifact lays each job's artifact under <root>/<artifact-name>/, where
    the name is notch-contract-<mode>-<runid>. We match the mode substring.
    """
    artifacts_root = Path(artifacts_root)
    jobs: dict = {}
    if not artifacts_root.is_dir():
        return jobs
    for child in sorted(artifacts_root.iterdir()):
        if not child.is_dir():
            continue
        matched = None
        for mode in JOB_LABELS:
            if f"-{mode}-" in child.name or child.name.endswith(f"-{mode}") or child.name == mode:
                matched = mode
                break
        if matched and matched not in jobs:
            jobs[matched] = (JOB_LABELS[matched], child)
    return jobs


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Build a single-file diagnostics report from CI artifacts")
    parser.add_argument("--artifacts-root", required=True, help="dir containing one subfolder per downloaded job artifact")
    parser.add_argument("--output", default="diagnostics-report.html")
    parser.add_argument("--template", default=str(Path(__file__).with_name("report_template.html")))
    parser.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", ""))
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", ""))
    parser.add_argument("--ref", default=os.environ.get("GITHUB_REF_NAME", ""))
    args = parser.parse_args(argv)

    jobs = discover_jobs(Path(args.artifacts_root))
    run = aggregate_run(jobs, {"id": args.run_id, "repository": args.repository, "ref": args.ref})
    html = render_report(run, Path(args.template))
    Path(args.output).write_text(html, encoding="utf-8")
    summary = run["summary"]
    print(
        f"diagnostics report: {args.output} jobs={summary['jobs']} "
        f"pass={summary['pass']} fail={summary['fail']} skip={summary['skip']} "
        f"error={summary['error']} result={summary['result']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
