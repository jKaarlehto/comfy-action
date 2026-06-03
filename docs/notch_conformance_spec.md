# Notch Contract Matrix — Specification and Generation

This is the **source of truth for the conformance case set** (test
orchestration the action owns). It names every negotiation axis, its components,
the counting criterion, and the exact case count, so that:

1. when extension behavior grows, you adjust the **axis spec** here and the case
   tables follow mechanically, and
2. the tests are **generated from the spec** — each row of a data table in
   `mock_client/` emits exactly one case; the totals below are the assertion.

The runnable action currently covers Layer 1: selection/discovery/readiness with
no workflow execution. The final target is a delivery round-trip suite, not a
thin standalone execution phase.

## Scope

This matrix is the **core integration contract**: **transport combinations**
(type × server × client reachability, hard and soft request) plus the
**file-manifest deployment-readiness gate**. That is all it tests.

It deliberately **excludes request-scoped feature/generation policies** —
output-metadata embedding (including `embed_file_manifest`), previews,
persisted-value load/save, content-addressed cache, replay data, and similar
`FeaturePolicy` toggles. Those are not transports and not gates; they are
output-shaping behaviors. They can be added as a separate policy sub-suite later
*if needed*, but they are not part of this conformance suite and must not inflate
its counts. Note the distinction: the file **manifest** appears here only as the
readiness **gate** (does the server have the required input files?), never as
the `embed_file_manifest` output **feature**.

> **Authority boundary.** The *contract itself* — what the transports mean, the
> negotiation rule, the readiness-gate semantics — is owned by the **ComfyUI-Notch
> repo** and documented there: `features.md` §"Transport Capability Negotiation",
> `docs/notch_comfy_client.md` §"File Manifest (Deployment Readiness)", and
> `core/data_types.py` (`OUTPUT_TYPES_BY_TRANSPORT`). This doc does **not**
> re-define the contract; it enumerates the **cases** that verify it and owns the
> **case count**, which is a testing decision (the boundary criterion in §3), not
> a property of the extension. Cross-repo references are by section/symbol name,
> not line number, because the extension's doc-drift process cannot reach this
> repo.

## 1. The model (summary; authority: `features.md` §Transport Capability Negotiation)

One rule, three **independent** axes:

```
usable = type-allowed ∩ server-available ∩ client-reachable
```

A hard-requested transport is honored **iff** it is in `usable`; otherwise the
server **hard-errors** — no silent downgrade. The client holds every fact up
front and decides locally.

**This formula governs only transport selection** — axes A1 (type-allowed), A2
(server-available), A3 (client-reachable), under request mode A4. The
deployment-readiness gate (A5) is **not** a term in this intersection: it is a
pre-execution gate, orthogonal to transport, counted **additively** and never
multiplying the transport matrix. The "Contract Matrix" name covers the verified
integration contract — transports plus the readiness gate — not transport
negotiation alone.

## 2. Axes and components

Components are what the case generator iterates. Grounding points to the
authoritative symbol/section in ComfyUI-Notch (no line numbers — verify by name).

| # | Axis | Components | Cardinality | Grounded in (ComfyUI-Notch, by symbol) |
|---|------|-----------|-------------|------------------------------|
| A0 | Setup / liveness | server wire-compat; WS handshake | 2 (fixed gates) | `core/client_compatibility.py`; `api/websocket.py` |
| A1 | **Type-allowed (T)** | `image`, `non-image` | 2 | `OUTPUT_TYPES_BY_TRANSPORT` in `core/data_types.py`. The axis exists **only because cuda is image-only**: the output type is what gates whether cuda is an allowed transport at all. So there are exactly two classes — `image` (→ cuda,disk,http) and `non-image` (→ disk,http) — and any concrete non-image type (FILE_3D_GLB, AUDIO, MESH, …) is one fixture realizing the same `non-image` class. |
| A2 | **Server-available (S)** | no-cuda: `{disk,http}`; cuda: `{cuda,disk,http}` | 2 meaningful classes | `supported_output_transports()` / `notch_feature_facts()` in `services/server_capabilities.py` |
| A3 | **Client-reachable / locality (C)** | reachable **sets**: `{cuda,disk,http}`, `{disk,http}`, `{http}` | 3 sets (regimes: local / route-disk / http-only) | `features.md` §"The transports"; helper is set-based per §Status "Client-interface set helper" |
| A4 | Request mode | `hard`, `soft` | 2 | `SelectOutputTransport` in `cpp/notch_comfy_client` (`m_preferredTransport` vs `m_preferenceOrder`) |
| A5 | **Deployment-readiness decision** (no execution) | all required files exist vs ≥1 missing | 2 | `/notch/get-required-files` (`api/file_manifest_handler.py`), `file_manifest_service.py`; C++ `ParseRequiredFilesResponse` → `RequiredFile.exists` |

**Out of scope (not axes):** request-scoped feature/generation policies —
`embed_file_manifest` output embedding, previews, persisted values,
content-addressed cache, replay data. They shape output, not transport, and are
deferred to an optional separate policy sub-suite (see §Scope).

**Key facts that keep the count small (and exact):**

- **A3 is set-based.** The helper never sees "local vs remote" — only the
  reachable *set* the Notch service computed. The 3 documented regimes collapse
  to **3 distinct sets**, all already used. So adding locality granularity (e.g.
  named-route disk) adds **0** selection cases as long as it reuses an existing
  reachable set. Locality's real cost is in delivery round-trip coverage.
- **A5 is a gate, not a transport.** Required-files is *user-actionable, not
  negotiable* (`notch_comfy_client.md` §File Manifest): a missing model file can't
  be auto-resolved. It is orthogonal to transport selection (not a term in
  `usable`). Layer 1 verifies only the **client-side readiness decision** — parse
  `/notch/get-required-files` and compute "any `exists=false` ⇒ not-ready"; it
  does **not** queue a run, so it stays pure/discovery. The matching
  **server-side run-block enforcement** (`not-ready ⇒ run rejected`) is **not yet
  built** and is a Layer-2 assertion; until then a live Layer-2 failure flags the
  gap to fix in ComfyUI-Notch.
- **A5 covers only the boolean gate boundary.** The two cases are *all required
  files exist* vs *at least one missing*. The gate is deliberately **not**
  multiplied by output type, transport, regime, or file kind (model vs other
  artifact) — those axes do not change the gate's decision rule.
- **`noop` is published but not selectable.** `supported_output_transports()`
  includes `noop` as a server-internal no-output mode, so it can appear in the S
  facts, but `/notch/inject` does not accept it as a delivery transport and
  `/notch/parse` never reports it on a connected output. It is therefore excluded
  from the A2 component set above and never enters `usable`.
- **The file manifest is the A5 gate, not a feature here.** `embed_file_manifest`
  (embedding the manifest into output metadata post-run) is a separate output
  *feature* and is **out of scope** for this matrix; only the readiness *gate*
  use of the manifest (does the server have the required input files?) is tested.
  The two are easy to conflate — they are not the same thing.

## 3. Counting criterion

**Boundary coverage with MC/DC-style isolation for the hard-selection
predicate.** The strict MC/DC claim applies only to the hard-selection rejects,
where each independent condition of `usable = T ∩ S ∩ C` is isolated so one axis
alone rules a transport out. The rest — setup/liveness, type-axis discovery,
soft fallback, the A5 gate, and final delivery — is equivalence-class /
decision-table boundary coverage, not strict MC/DC. Applied as follows:

- every transport is **selected** at least once, per type class where it varies;
- every transport is **rejected** at least once by **each independent axis** that
  can exclude it;
- the **empty-intersection** reject once;
- each distinct **soft** behavior once;
- each **gate** component (ready / not-ready) once;
- final delivery tests each **reachable live delivery** `(type × transport × regime)` once, plus
  representative server-side delivery rejects.

A full Cartesian product would be exact too, but most rows would be redundant;
boundary coverage is the minimal exact set and is what the contract docs already
use in their worked examples.

## 4. Exact case counts

### Layer 1 — Selection + Discovery (pure; single server; **no execution**) = 19

| Component | Axes | Count | Derivation |
|---|---|---|---|
| Setup / liveness | A0 | 2 | wire-compat + WS handshake |
| Type-axis discovery (live `/notch/parse`) | A1 | 2 | `|T|` = {image, non-image} |
| Deployment-readiness decision (live `/notch/get-required-files`; no execution) | A5 | 2 | all required files exist vs ≥1 missing |
| Hard selection | A1·A2·A3·A4 | 10 | 5 positives + 5 rejects (below) |
| Soft selection | A4 | 3 | canonical order `cuda>http>disk`: top-usable chosen, http-over-disk when no cuda, and the debug-mask path forcing disk |

Hard = **10**: positives `cuda(1) + disk×{img,nonimg}(2) + http×{img,nonimg}(2) = 5`;
rejects `cuda-by-T(1) + cuda-by-S(1) + cuda-by-C(1) + disk-by-C(1) + empty(1) = 5`.
The cuda reject is isolated to one axis per row, so **S**-unavailable (no GPU)
and **C**-unreachable (remote) are distinct cases.

### Layer 2 — Delivery Round-Trip (planned) = 13

| Component | Count | Derivation |
|---|---|---|
| Live delivery positives | 11 | `local`: img{cuda,disk,http}=3 + nonimg{disk,http}=2; `route-disk`: img{disk,http}=2 + nonimg{disk,http}=2; `http-only`: img{http}=1 + nonimg{http}=1 |
| Server delivery hard-error negatives | 2 | cuda-without-GPU + type/transport mismatch |

Layer 2 proves the **transports actually deliver** and that the gate/negatives
hold at runtime. It is a real round-trip:

```text
NotchSingleInput fixture value
  -> /notch/inject execute=true
  -> workflow executes
  -> NotchOutputNode delivers through the selected transport
  -> mock client receives/fetches/imports the output
  -> SHA-256 plus decoded shape/type checks match expectations
```

Do not create or preserve a permanent queue-only "execution" layer. A workflow
that only proves `/notch/inject?execute=true` can reach a terminal WebSocket
event has no `NotchSingleInput`, no `NotchOutputNode`, no selected transport,
and no byte verification. It is useful while developing the harness and should
be deleted once the delivery round-trip exists.

Topology is part of delivery, not selection:

- **delivery-local** runs in the existing single container. The mock client and
  ComfyUI share one filesystem and one CUDA process namespace. Test disk, HTTP,
  and CUDA here. CUDA IPC belongs only here; do not attempt cross-container CUDA
  IPC as a contract test.
- **delivery-remote** runs in Docker Compose with separate client/server
  containers. No shared filesystem by default. Test HTTP, named-route disk when
  explicitly configured, and unshared-filesystem negatives. CUDA is absent from
  client reachability in this topology.

Both delivery jobs depend on `extension-boot` and `protocol-negotiation`. They
must not depend on any queue-only interim check. Successful execution and
file-availability enforcement are preconditions/negative cases inside delivery,
not separate prerequisite jobs.

Layer 2 does **not** include output feature/generation policies (metadata
embedding, manifest embedding, previews, etc.) — those are out of scope per
§Scope and would be a separate sub-suite if ever added.

### Totals (exact)

```
implemented Layer 1        = 19
planned full conformance   = 19 + 13 = 32
```

## 5. Growth rules — how the count changes when behavior grows

Edit §2, then the tables in `mock_client/` follow:

- **New output type** → if it introduces a *new* transport set, +1 type-axis case
  and new selection rows; if it reuses an existing set, +1 type-axis case only.
- **New transport** → new positives (per type class allowing it) + one reject per
  axis that can exclude it.
- **New reachability regime (locality)** → **+0** to selection if it reuses an
  existing reachable set; only adds delivery round-trip cases (and topology).
- **New capability gate** (like A5) → +2 (pass/fail of the gate), additive.
- **New output feature/generation policy** (metadata, manifest embedding,
  previews, …) → **out of scope** for this matrix; if ever needed it becomes a
  separate policy sub-suite, additive — it never multiplies the transport matrix.

## 6. Grounding checklist — each case-group to extension source (by symbol)

| Case group | Tests | Grounded in (ComfyUI-Notch) | Grounding strength |
|---|---|---|---|
| wire-compat | client/server protocol versions agree | `core/client_compatibility.py`; C++ `CheckServerCompatibility` | live |
| WS handshake | `/ws` accepts feature_flags, sends catch-up | `api/websocket.py`; `register_server_feature_flags` in `comfy_adapter/runtime.py` | live (shallow) |
| type-axis | `/notch/parse` `outputs[].transports` per type | `get_workflow_outputs` in `services/notch_workflow_graph.py`; `OUTPUT_TYPES_BY_TRANSPORT` | live |
| readiness decision (L1) | required files `exists`; client-side "any missing ⇒ not-ready" decision | `api/file_manifest_handler.py`; `file_manifest_service.py`; C++ `ParseRequiredFilesResponse` | live (client decision) |
| readiness enforcement (delivery) | `not-ready ⇒ server rejects the run` | (not yet built in ComfyUI-Notch) | planned delivery negative |
| hard/soft selection | `usable = T∩S∩C`, hard-error, no downgrade | C++ `SelectOutputTransport`; expected sets from `OUTPUT_TYPES_BY_TRANSPORT`, `supported_output_transports()`, `build_notch_output_config` in `services/output_config_service.py` | pure helper (server-grounded expectations) |
| delivery round-trip | actual disk/http/cuda delivery + events + byte verification | `services/output_delivery.py`; `services/cuda_shares.py`; `notch-output-ready` | planned |

"pure helper" means the case exercises the **C++ interface** logic; its *expected
values* come from extension source, but it does not yet prove the live server
agrees at delivery time — that is the planned delivery round-trip job.

## 7. Generation — spec to tests

The cases are generated from data, not hand-written per test:

- **Selection** — `mock_client/src/matrix.cpp` `kHardRows` (10) and `kSoftRows`
  (3). Each row is one `(typeAllowed, serverAvailable, clientReachable,
  requiredOrPreference, expect)` tuple → one case. Add a row = add a case. Hard
  rows set a single required transport; soft rows pass the canonical client
  preference order `cuda>http>disk` (see §Negotiation in ComfyUI-Notch
  `features.md`), and the debug-mask soft row narrows `clientReachable` to force
  disk.
- **Type-axis** — one case per output in the `/notch/parse` fixture; expected
  transports come from `ExpectedTypeTransports()` (image → cuda,disk,http; else →
  disk,http).
- **Readiness** — one case per required-files fixture (`requiredFilesReadyJson`,
  `requiredFilesMissingJson`); gate = "any `exists=false` ⇒ blocked".
- **Setup/WS** — fixed.

`mock_client/tests/stub_check.cpp` runs the whole matrix against stub transports
(no server, no IXWebSocket, no network) and asserts the total. **It is the
executable form of §4** — change the spec, change the asserted count. Today:

```
stub matrix: pass=19 fail=0 skip=0 error=0   ← Layer 1 exact
```

### Artifact shape

Each case writes `conformance/cases/NNN-<id>/case.json`, **prettified** and
**self-describing**, so the evidence reads without the source:

- `title` — plain one-line summary of what the case checks.
- `phase` — `liveness` | `discovery` | `readiness` | `transport-selection`.
- `description` — why the case exists, in prose.
- `spec_ref` — pointer back into this document.
- the request: `requested_transport` (a single required transport, never
  downgraded) or `preferred_order` (first usable wins) for selection cases.
- `expected` vs `actual`, `result`, `errors`.

The run also writes `conformance/INDEX.md` — every case grouped by phase
with title + result — as the human entry point. The `.json` files are
pretty-printed; the run-wide `.jsonl` streaming logs (`mock-client.jsonl`,
`http.jsonl`, `websocket.jsonl`) stay one record per line (the JSON Lines
contract — read line by line, not pretty-printed).

## 8. Status — what is real today vs. the ideal

Be honest about what the action proves. The runnable public jobs are:

```text
extension-boot -> protocol-negotiation
```

`extension-boot` proves the extension loads and the C++ client interface
compiles. `protocol-negotiation` proves Layer 1: discovery, readiness decision,
transport selection, and the WebSocket handshake. It does not execute a workflow.

There is currently **no test of the actual Notch round-trip**: injecting an input
value, executing, delivering an output, and verifying the delivered bytes. Input
injection, output delivery, and byte verification are all untested.

Do not add a permanent standalone execution job. A workflow that only proves
`/notch/inject?execute=true` can reach a terminal WebSocket event has no
`NotchSingleInput`, no `NotchOutputNode`, no selected transport, and no byte
verification. It may be useful while developing the harness, but the final
delivery implementation should delete those queue-only fixtures or fold their
assertions into delivery preconditions. The compose delivery jobs must not depend
on them.

Total implemented live count today: **19 negotiation**. These prove protocol
negotiation; they do not prove delivery.

## 9. Delivery Diagnostics — WS assertions and per-case Notch logs (planned)

Delivery cases execute workflows, so each case gains two diagnostic channels.
Both are **structured, never regex over the server log** — the ComfyUI log format
is not a stable contract, so the matrix must not assert on parsed log text. Raw
log is kept only as a per-case byte-offset slice (`server.log`) for human
context.

### 9a. WebSocket lifecycle assertions (client-side, no new server work)

The mock client already holds the `/ws` connection and the C++ interface already
parses lifecycle events keyed by `prompt_id` (`ParseWebSocketEvent` →
`EventExecutionSuccess` / `EventExecutionError` / `EventExecutionInterrupted` /
`EventExecuted`). Per execution case:

1. Submit via `/notch/inject`; read `prompt_id` from the inject response
   (`ParseWorkflowSubmissionResponse`).
2. Wait for the matching terminal event on `/ws`.
3. Assert it: `execution_success` for positive cases; `execution_error` (with
   `exception_type`, `node_id`, `traceback`) for cases that must fail at runtime.
4. Record the lifecycle to `websocket.jsonl` and the terminal outcome to
   `case.json`.

Pre-queue validation failures arrive as structured JSON in the inject HTTP
response (`error`, `node_errors`) — also no log parsing.

### 9b. Per-case Notch diagnostics (warnings/errors as context)

A case can **pass while the server emitted warnings** (e.g. a cache miss, a
proceed-without-X path). Those warnings are valuable context, captured
structurally via a planned **ComfyUI-Notch** feature (extension-side, documented
in `docs/notch_integration_guide.md` *when built* — not before):

- **`NOTCH_CI` env flag** (distinct from `NOTCH_DEBUG`, which is DEBUG-level
  stdout spew): attaches a structured capture handler at `WARNING+` to the
  `"Notch"` logger (the single facade in `utils/logger.py`). It buffers
  `LogRecord` fields — `{level, logger/module/func, message, prompt_id, ts,
  exc_info}` — as objects, no text parsing.
- **`prompt_id` contextvar** set at inject/execution start; the handler stamps
  each record. Reliable on the execution thread (ComfyUI runs prompts serially);
  background-thread logs (drop-on-full side effects) stamp `prompt_id=null`,
  best-effort.
- **Pull endpoint `GET /notch/diagnostics?prompt_id=…`** returns that prompt's
  buffered records. The matrix queries it after each case and writes them to the
  case directory (e.g. `notch-diagnostics.json`). Buffer is bounded (last-N
  prompts / cleared on inject) so a long-lived server does not grow unbounded.
- Facade tweak: `warning()`/`error()` accept `exc_info` so tracebacks attach
  (today only `exception()` carries them).

**Consumption rule:** diagnostics are **context, not pass/fail**. A case's
verdict comes from its expected outcome (selection / gate / WS terminal event);
the diagnostics are attached evidence. The matrix may optionally flag an
*unexpected* error-level record on an otherwise-passing case, but warnings never
fail a case on their own.

### 9c. Prerequisite review sweep (extension-side)

For 9b's signal to be meaningful, ComfyUI-Notch's existing log sites need a level
pass (separate work, judgement per site — a scout-orchestrator-editor sweep, not
find/replace):

- **Promote to `warning`:** missing required file, cache miss/eviction, CUDA
  unavailable-but-proceeding, any "proceeding without X" branch.
- **Promote to `error`/`exception` (with `exc_info`):** swallowed exceptions that
  are not intentional hot-path drops.
- **Leave as-is:** intentional drop-on-full async side effects and sub-threshold
  perf logs (per the project's existing logging rules).

The codebase already routes through the logger (only ~3 cosmetic `print()`s), so
this is a level-correctness pass over ~65 contract-relevant branch points and the
swallowing `except` handlers, not a print-to-log migration.
