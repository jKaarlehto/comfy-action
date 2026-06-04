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
                "diagnostics": _summarize_case_diagnostics(evidence.get("notch_diagnostics")),
                "evidence": evidence,
            }
        )
    return cases


def _summarize_records(records: list, ci_enabled: bool = True) -> dict | None:
    """Structure a list of NOTCH_CI records into an ordered event breadcrumb plus a
    warning list. Records are {kind:"event"|"log", event/name, message, severity}.
    Events are CI decision breadcrumbs; logs are WARNING+ context. Evidence, not a
    verdict (spec §9b).
    """
    if not isinstance(records, list) or not records:
        return None
    events: list[str] = []
    warnings: list[dict] = []
    for record in records:
        if not isinstance(record, dict):
            continue
        kind = str(record.get("kind", ""))
        if kind == "event":
            name = str(record.get("event") or record.get("name") or "")
            if name:
                events.append(name)
        else:
            warnings.append(
                {
                    "severity": str(record.get("severity", "") or kind or "LOG"),
                    "message": str(record.get("message") or record.get("event") or record.get("name") or ""),
                }
            )
    return {"ci_enabled": ci_enabled, "count": len(records), "events": events, "warnings": warnings}


def _summarize_case_diagnostics(diag: object) -> dict | None:
    """Structure a case's GET /notch/diagnostics response (has a top-level
    'records' list) for the per-case panel.
    """
    if not isinstance(diag, dict):
        return None
    summary = _summarize_records(diag.get("records") or [], ci_enabled=bool(diag.get("ci_enabled")))
    if summary is not None and "count" in diag:
        summary["count"] = int(diag.get("count") or summary["count"])
    return summary


def _diagnostics_by_prompt(job_dir: Path) -> dict:
    """Read the run-level NOTCH_CI stream (notch-ci.jsonl) and group its records by
    prompt_id, so each prompt's breadcrumb can be attached to its case.

    The stream lives at the job root (local single-container) or under server/
    (remote, where ComfyUI runs in the server container). Records with no prompt_id
    (startup/unattributed) are grouped under "".
    """
    ci_log = job_dir / "notch-ci.jsonl"
    if not ci_log.is_file():
        ci_log = job_dir / "server" / "notch-ci.jsonl"
    grouped: dict = {}
    if not ci_log.is_file():
        return grouped
    for line in _read_text(ci_log).splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            record = json.loads(line)
        except Exception:
            continue
        if not isinstance(record, dict) or not (record.get("severity") or record.get("kind")):
            continue
        grouped.setdefault(str(record.get("prompt_id", "")), []).append(record)
    return grouped


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

    # Totals/result come from conformance-result.json, but fall back to
    # compatibility-result.json (the python wrapper copies the conformance totals
    # there and records the overall result) — important when the mock client
    # exits before writing its own result file.
    compat = _read_json(job_dir / "compatibility-result.json") or {}
    totals = result.get("totals", {}) if isinstance(result, dict) else {}
    if not totals and isinstance(compat, dict):
        totals = compat.get("conformance", {}) or {}
    # When no result file recorded totals, derive them from the case records so the
    # report still reflects what actually ran.
    cases = _normalize_cases(job_dir)
    if not totals and cases:
        counter = {"pass": 0, "fail": 0, "skip": 0, "error": 0}
        for case in cases:
            counter[case.get("result", "error")] = counter.get(case.get("result", "error"), 0) + 1
        totals = counter
    result_str = (
        (result.get("result") if isinstance(result, dict) else "")
        or (compat.get("result") if isinstance(compat, dict) else "")
        or (boot.get("result") if isinstance(boot, dict) else "")
        or ""
    )

    # Attribute the run-level NOTCH_CI stream to each case by prompt_id: each
    # prompt's breadcrumb belongs to its case, not in one giant job-level blob.
    # The per-case GET /notch/diagnostics (case["diagnostics"]) is preferred when
    # present; otherwise build it from the prompt's slice of the stream.
    ci_by_prompt = _diagnostics_by_prompt(job_dir)
    attributed_prompts: set = set()
    for case in cases:
        actual = case.get("actual") if isinstance(case.get("actual"), dict) else {}
        prompt_id = str(actual.get("prompt_id", "")) if isinstance(actual, dict) else ""
        if prompt_id:
            attributed_prompts.add(prompt_id)
        if not case.get("diagnostics") and prompt_id and prompt_id in ci_by_prompt:
            case["diagnostics"] = _summarize_records(ci_by_prompt[prompt_id])
    # The job-level panel keeps only records that belong to no case (startup /
    # unattributed), so it is small context rather than the whole 447-record dump.
    unattributed: list[dict] = []
    for prompt_id, records in ci_by_prompt.items():
        if prompt_id in attributed_prompts:
            continue
        for record in records:
            unattributed.append(
                {
                    "severity": str(record.get("severity", "")),
                    "kind": str(record.get("kind", "")),
                    "name": str(record.get("name") or record.get("event") or ""),
                    "message": str(record.get("message", "")),
                    "prompt_id": prompt_id,
                }
            )

    return {
        "name": name,
        "label": label,
        "result": result_str,
        "totals": totals,
        "cases": cases,
        "environment": environment,
        "cuda": _read_json(job_dir / "cuda-diagnostics.json"),
        "server_cuda": _read_json(job_dir / "server" / "cuda-diagnostics.json"),
        "diagnostics": unattributed,
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


def _result_icon(result: str) -> str:
    return {"pass": "✅", "fail": "❌", "error": "🟥", "skip": "⏭️"}.get(result, "•")


def render_run_markdown(run: dict) -> str:
    """Render an aggregate markdown overview of the whole run (every job's totals
    plus the run rollup), for the report job's GitHub Step Summary — the same kind
    of at-a-glance md the per-job conformance/CUDA summaries provide, but spanning
    all jobs. The HTML report remains the deep drill-down.
    """
    summary = run.get("summary", {})
    banner = "✅ PASS" if summary.get("result") == "pass" else "❌ FAIL"
    lines = [
        f"## Conformance run — {banner}",
        "",
        f"**jobs={summary.get('jobs', 0)} · pass={summary.get('pass', 0)} · "
        f"fail={summary.get('fail', 0)} · skip={summary.get('skip', 0)} · "
        f"error={summary.get('error', 0)}**",
        "",
        "| job | result | pass | fail | skip | error |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for job in run.get("jobs", []):
        totals = job.get("totals", {})
        lines.append(
            f"| {job.get('label', job.get('name', ''))} | {_result_icon(job.get('result', ''))} "
            f"{job.get('result', '') or '—'} | {totals.get('pass', 0)} | {totals.get('fail', 0)} | "
            f"{totals.get('skip', 0)} | {totals.get('error', 0)} |"
        )
    lines.append("")
    # Surface failing cases by name so the run page shows what broke without the HTML.
    failing = [
        (job.get("label", job.get("name", "")), case)
        for job in run.get("jobs", [])
        for case in job.get("cases", [])
        if case.get("result") in ("fail", "error")
    ]
    if failing:
        lines.append("### Failing cases")
        for label, case in failing:
            errors = ", ".join(case.get("errors", []) or [])
            suffix = f" — `{errors}`" if errors else ""
            lines.append(f"- ❌ `{case.get('id', '')}` ({label}){suffix}")
        lines.append("")
    lines.append(
        "The full single-file `diagnostics-report.html` (every job, all cases, logs, "
        "diagnostics) is attached as the **diagnostics-report** artifact — download and open directly."
    )
    return "\n".join(lines) + "\n"


_JOB_MARKERS = ("conformance-result.json", "compatibility-result.json", "extension-boot-result.json")


def _mode_for_job_dir(job_dir: Path) -> str:
    """Identify a job dir's mode: prefer the recorded profile, fall back to the name.

    compatibility-result.json carries a 'profile' like 'delivery_remote_client' /
    'delivery_remote_server'; strip the _client/_server suffix to the job mode.
    """
    compat = _read_json(job_dir / "compatibility-result.json") or {}
    profile = str(compat.get("profile", "")) if isinstance(compat, dict) else ""
    for suffix in ("_client", "_server"):
        if profile.endswith(suffix):
            profile = profile[: -len(suffix)]
    if profile in JOB_LABELS:
        return profile
    name = job_dir.name
    for mode in JOB_LABELS:
        if f"-{mode}-" in name or name.endswith(f"-{mode}") or name == mode:
            return mode
    return ""


def discover_jobs(artifacts_root: Path) -> dict:
    """Find each job's artifact folder under artifacts_root, robust to how
    download-artifact lays things out (one subdir per artifact, possibly nested,
    possibly renamed).

    A job dir is any directory that directly contains a result marker or a
    conformance/cases tree. We search a few levels deep and fold the remote
    delivery's server/ subdir into its parent (it is not a separate job).
    """
    artifacts_root = Path(artifacts_root)
    jobs: dict = {}
    if not artifacts_root.is_dir():
        return jobs

    def is_job_dir(path: Path) -> bool:
        if path.name == "server":  # the remote server's artifacts belong to its parent job
            return False
        return any((path / marker).is_file() for marker in _JOB_MARKERS) or (
            path / "conformance" / "cases"
        ).is_dir()

    candidates: list[Path] = []
    # download-artifact extracts a SINGLE artifact's contents directly into the
    # download path (flattened), but nests one subdir per artifact when several
    # match. Handle both: if the root itself is a job dir, use it; otherwise scan
    # descendants for per-artifact subdirs.
    if is_job_dir(artifacts_root):
        candidates.append(artifacts_root)
    else:
        seen: set = set()
        stack = [(artifacts_root, 0)]
        while stack:
            current, depth = stack.pop()
            if current in seen or depth > 4:
                continue
            seen.add(current)
            if current.name == "server":
                continue
            if current != artifacts_root and is_job_dir(current):
                candidates.append(current)
                continue  # do not descend into a job dir
            for child in sorted(current.iterdir()):
                if child.is_dir():
                    stack.append((child, depth + 1))

    for job_dir in sorted(candidates, key=lambda p: p.name):
        mode = _mode_for_job_dir(job_dir)
        if mode and mode not in jobs:
            jobs[mode] = (JOB_LABELS[mode], job_dir)
    return jobs


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Build a single-file diagnostics report from CI artifacts")
    parser.add_argument("--artifacts-root", required=True, help="dir containing one subfolder per downloaded job artifact")
    parser.add_argument("--output", default="diagnostics-report.html")
    parser.add_argument("--summary-output", default="diagnostics-summary.md",
                        help="aggregate markdown overview for the run's Step Summary")
    parser.add_argument("--template", default=str(Path(__file__).with_name("report_template.html")))
    parser.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", ""))
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", ""))
    parser.add_argument("--ref", default=os.environ.get("GITHUB_REF_NAME", ""))
    args = parser.parse_args(argv)

    jobs = discover_jobs(Path(args.artifacts_root))
    run = aggregate_run(jobs, {"id": args.run_id, "repository": args.repository, "ref": args.ref})
    html = render_report(run, Path(args.template))
    Path(args.output).write_text(html, encoding="utf-8")
    if args.summary_output:
        Path(args.summary_output).write_text(render_run_markdown(run), encoding="utf-8")
    summary = run["summary"]
    print(
        f"diagnostics report: {args.output} jobs={summary['jobs']} "
        f"pass={summary['pass']} fail={summary['fail']} skip={summary['skip']} "
        f"error={summary['error']} result={summary['result']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
