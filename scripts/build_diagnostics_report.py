#!/usr/bin/env python3
"""Build one self-contained diagnostics-report.html from extracted CI artifacts.

The report aggregates every conformance job's artifact folder into a single
portable HTML file: all data is inlined (no sibling files, no server, no network
at view time), so a developer can download the one file and open it via file://.

This module is stdlib-only on purpose — it runs in the report aggregation job with
no extra toolchain. The dynamic payload is one normalized JSON blob inlined into a
static template; the security-critical part is escaping that JSON so it cannot
break out of its <script> element (see json_for_script).
"""

from __future__ import annotations

import json


def json_for_script(payload: object) -> str:
    """Serialize payload for safe embedding in a <script type="application/json"> block.

    Escapes the sequences that could break out of the script element or corrupt
    the inlined JSON when the browser reads textContent:
      <  -> \\u003c   (neutralizes </script>, <!--, <script — every '<')
      U+2028 / U+2029 -> \\u2028 / \\u2029 (valid in JSON, illegal in JS string literals)
    The result is still valid JSON: replacing \\u003c back to '<' yields the
    original document, so JSON.parse(textContent) recovers payload.
    """
    text = json.dumps(payload, ensure_ascii=False, sort_keys=True)
    text = text.replace("<", "\\u003c")
    text = text.replace(" ", "\\u2028")
    text = text.replace(" ", "\\u2029")
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
