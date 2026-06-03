import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from build_diagnostics_report import json_for_script, truncate_log  # noqa: E402


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
