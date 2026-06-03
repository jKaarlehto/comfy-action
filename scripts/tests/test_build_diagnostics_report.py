import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from build_diagnostics_report import (  # noqa: E402
    _summarize_case_diagnostics,
    aggregate_run,
    discover_jobs,
    json_for_script,
    normalize_job,
    render_report,
    truncate_log,
)

_HERE = os.path.dirname(__file__)
TEMPLATE = os.path.join(_HERE, "..", "report_template.html")
FIXTURE_ROOT = os.path.join(_HERE, "fixtures", "artifacts")
FIXTURE_JOB = os.path.join(FIXTURE_ROOT, "notch-contract-delivery_local-1")
DATA_RE = re.compile(r'<script type="application/json" id="diagnostics-data">(.*?)</script>', re.S)


def test_escapes_script_close_tag():
    out = json_for_script({"x": "</script><script>alert(1)</script>"})
    assert "</script>" not in out
    assert "\\u003c" in out


def test_escapes_angle_brackets_and_line_separators():
    out = json_for_script({"x": "a<b", "u": "x y z"})
    assert "<" not in out
    assert " " not in out
    assert " " not in out


def test_round_trips_as_json():
    payload = {"a": 1, "nested": {"b": [1, 2, 3]}, "s": "</script>", "u": "x y"}
    out = json_for_script(payload)
    # The browser JSON.parses textContent; unescaping < yields valid JSON again.
    assert json.loads(out.replace("\\u003c", "<")) == payload


def test_small_log_kept_whole():
    lines = [f"line {i}" for i in range(10)]
    result = truncate_log("\n".join(lines), head=50, tail=50)
    assert result["truncated"] is False
    assert result["head"] == lines
    assert result["tail"] == []
    assert result["total_lines"] == 10


def test_large_log_head_and_tail():
    lines = [f"line {i}" for i in range(1000)]
    result = truncate_log("\n".join(lines), head=20, tail=20)
    assert result["truncated"] is True
    assert result["head"] == lines[:20]
    assert result["tail"] == lines[-20:]
    assert result["total_lines"] == 1000


def test_normalize_job_reads_cases_and_env():
    job = normalize_job("delivery_local", "Local delivery", FIXTURE_JOB)
    assert job["name"] == "delivery_local"
    assert job["result"] == "fail"
    assert job["totals"]["fail"] == 1
    ids = [c["id"] for c in job["cases"]]
    assert "local.image.cuda.deliver" in ids
    assert "local.audio.cuda.reject-type" in ids
    failing = next(c for c in job["cases"] if c["result"] == "fail")
    assert failing["errors"] == ["unexpected_accept"]
    assert failing["expected"] == {"selected": False}
    assert job["environment"]["platform"] == "Linux-WSL2"
    assert job["environment"]["torch"] == "2.12.0+cu130"


def test_aggregate_run_rolls_up_and_orders():
    jobs = discover_jobs(os.path.abspath(FIXTURE_ROOT))
    assert "delivery_local" in jobs
    run = aggregate_run(jobs, {"id": "123", "repository": "r", "ref": "v0.3.0"})
    assert run["summary"]["jobs"] == 1
    assert run["summary"]["result"] == "fail"  # one job has a fail
    assert run["generated_at"]
    assert run["jobs"][0]["name"] == "delivery_local"


def test_render_single_self_contained_file():
    run = {"generated_at": "t", "run": {"id": "1"}, "summary": {"result": "pass", "jobs": 0}, "jobs": []}
    html = render_report(run, TEMPLATE)
    assert 'src="http' not in html and 'href="http' not in html
    assert "fetch(" not in html
    assert 'src="./' not in html and 'href="./' not in html
    match = DATA_RE.search(html)
    assert match
    assert "</script>" not in match.group(1)  # the inlined data has no raw close tag
    assert json.loads(match.group(1).replace("\\u003c", "<")) == run


def test_render_escapes_data_breakout():
    run = {"generated_at": "t", "run": {}, "summary": {}, "jobs": [{"name": "x", "label": "</script><img src=x>", "cases": []}]}
    html = render_report(run, TEMPLATE)
    match = DATA_RE.search(html)
    # a data-injected </script> or < must never appear raw inside the data block
    assert "</script>" not in match.group(1)
    assert "<img" not in match.group(1)


def test_per_case_diagnostics_structured():
    diag = {
        "ci_enabled": True,
        "count": 4,
        "records": [
            {"kind": "event", "event": "inject.request_parsed"},
            {"kind": "event", "event": "output_node.delivery_done"},
            {"kind": "log", "severity": "WARNING", "message": "cache miss for key x"},
            {"kind": "log", "message": "no severity log"},
        ],
    }
    summary = _summarize_case_diagnostics(diag)
    assert summary["ci_enabled"] is True
    assert summary["count"] == 4
    assert summary["events"] == ["inject.request_parsed", "output_node.delivery_done"]
    assert len(summary["warnings"]) == 2
    assert summary["warnings"][0]["severity"] == "WARNING"
    assert "cache miss" in summary["warnings"][0]["message"]
    # non-dict / missing records -> None (no panel)
    assert _summarize_case_diagnostics(None) is None
    assert _summarize_case_diagnostics({"count": 0}) is None


def test_full_build_from_fixture_root(tmp_path=None):
    import tempfile

    out = os.path.join(tempfile.mkdtemp(), "diagnostics-report.html")
    jobs = discover_jobs(os.path.abspath(FIXTURE_ROOT))
    run = aggregate_run(jobs, {"id": "T"})
    html = render_report(run, TEMPLATE)
    with open(out, "w", encoding="utf-8") as handle:
        handle.write(html)
    assert os.path.getsize(out) > 0
    body = open(out, encoding="utf-8").read()
    assert "local.image.cuda.deliver" in body  # case id reachable in the document


if __name__ == "__main__":
    # Allow running without pytest: execute every test_* and report.
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
            except AssertionError as exc:
                failures += 1
                print(f"FAIL {name}: {exc}")
            else:
                print(f"ok   {name}")
    raise SystemExit(1 if failures else 0)
