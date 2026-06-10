# Notch Contract Matrix — Specification and Generation

This is the **source of truth for the conformance case set** (test
orchestration the action owns). It names every negotiation axis, its components,
the counting criterion, and the exact case count, so that:

1. when extension behavior grows, you adjust the **axis spec** here and the case
   tables follow mechanically, and
2. the tests are **generated from the spec** — each row of a data table in
   `mock_client/` emits exactly one case; the totals below are the assertion.

The runnable action covers Layer 1 selection/discovery/readiness and Layer 2
delivery round trips. The public jobs compose in order:

```text
extension_boot -> protocol_negotiation -> delivery_local -> delivery_remote
```

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
| A4 | Request mode | `hard`, `soft` | 2 | `SelectOutputTransport` in `cpp/comfy_extension_client` (`m_requiredTransport` vs `m_preferenceOrder`) |
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
- **`cuda_device_index` is one server fact feeding two axes, not its own axis.**
  The server publishes it (`extension.notch.cuda_device_index`, via
  `notch_feature_facts()`), and two distinct readings come off it:
  (1) its **existence** gates **A2** — `cuda_enabled()` is `cuda_device_index ≥ 0`
  and `supported_output_transports()` adds `cuda` only then, so
  `cuda ∈ S ⟺ cuda_device_index ≥ 0`;
  (2) its **value/identity** feeds **A3** — per `features.md` §"The transports",
  cuda reachability is "local + device-index match": the client includes `cuda`
  in C only if it is local *and* its own device index equals the server's. That
  match is computed client-side and baked into the reachable set before the
  helper sees it (the helper never reads the index). The suite exercises the
  existence side (the cuda skip gate + the share-status index used for
  `cudaSetDevice`); the **device-mismatch** branch of C (server device N, client
  can't see device N ⇒ cuda unreachable) is **untested**, as it needs a
  multi-GPU host.
- **A5 is a gate, not a transport.** Required-files is *user-actionable, not
  negotiable* (`notch_comfy_client.md` §File Manifest): a missing model file can't
  be auto-resolved. It is orthogonal to transport selection (not a term in
  `usable`). Layer 1 verifies only the **client-side readiness decision** — parse
  `/notch/get-required-files` and compute "any `exists=false` ⇒ not-ready"; it
  does **not** queue a run, so it stays pure/discovery. Server-side run-block
  enforcement is not a transport-selection axis and is not part of this
  conformance count.
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

## 3. Coverage model and counting criterion

This section is the formal justification for every count in §4. It states the
model, proves which transports each axis can exclude, and derives the exact
case set from a stated coverage criterion. Counts are consequences here, not
assertions.

### 3.1 Model

Let **Σ = {cuda, disk, http}** be the selectable delivery transports. (`noop` is
published in server facts but is not selectable — see §2 — so `noop ∉ Σ`.) A
deployment presents three eligibility sets, each `⊆ Σ`, whose **realizable**
values are exactly (from §2):

| set | realizable values |
|---|---|
| **T** type-allowed | image ⟹ `{cuda,disk,http}`; non-image ⟹ `{disk,http}` |
| **S** server-available | GPU ⟹ `{cuda,disk,http}`; no-GPU ⟹ `{disk,http}` |
| **C** client-reachable | **deployment regimes:** local ⟹ `{cuda,disk,http}`; route-disk ⟹ `{disk,http}`; http-only ⟹ `{http}`. **Plus client masking** (below). |

T is fixed by the workflow/output type; S by the server's hardware; C by the
client's deployment regime. C has one extra realizable source: the client may
**mask** its own reachability to *force* a transport — e.g. the debug flow masks
cuda+http to leave `C={disk}` so a dataflow is traced over an inspectable medium
(`features.md` §"Required transport vs preference order"). Masked sets are
realizable but are *not* deployment regimes; A3's "3 sets" enumerates the
regimes, and masking can present any non-empty subset.

`usable = T ∩ S ∩ C`. A **hard** request `r` is honored iff `r ∈ usable`, else
rejected (no downgrade). A **soft** preference order `ρ` returns the first
element of `ρ` that is in `usable`; if none is, it returns the first element of
`usable` (the *fallback*).

### 3.2 Excludability lemmas (these determine the case set)

Over the realizable values above:

- **Lemma H (http-universality, deployment regimes).** `http ∈ T` and `http ∈ S`
  for every realizable T, S; and `http ∈ C` for all three **deployment regimes**.
  Therefore, absent client masking, `http ∈ usable` always and **`usable` is
  never empty**. (`features.md` calls http "the universal remote transport".)
  Client masking (§3.1) is the only way C omits http; a *sensible* client masks
  only to force a transport that is still usable (e.g. `{disk}`), so it also
  never yields an empty `usable`.
- **disk** satisfies `disk ∈ T` and `disk ∈ S` for all realizable T, S; `disk ∈
  C` iff C is not the http-only regime. So **disk is excludable by exactly one
  axis: C.**
- **cuda** satisfies `cuda ∈ T` iff image, `cuda ∈ S` iff GPU, `cuda ∈ C` iff
  local. So **cuda is excludable by three independent axes: T, S, and C.**

**Corollary (empty-set needs a degenerate input).** By Lemma H, `usable = ∅`
cannot arise from any deployment regime, nor from any *sensible* client mask. It
requires a degenerate input no correct client constructs — a server or mask set
disjoint from `T∩S`, e.g. the test's `S={cuda}, C={cuda}` with a non-image type.
The `empty-intersection` case is therefore a **defensive robustness test of the
helper's empty branch**, not a realizable negotiation, and is counted as such
(not as an "axis reject").

### 3.3 Criterion

**Hard selection: MC/DC on the honored predicate, per transport.**
`honored(x) = (x∈T) ∧ (x∈S) ∧ (x∈C)` is a three-condition AND. MC/DC requires the
all-true case plus, for each condition, a case where that condition alone is
false (others true), flipping the decision to reject.

- **cuda** — all three conditions are free ⟹ **full MC/DC = 4 cases**: 1 honored
  (image·GPU·local) + 3 rejects isolating ¬T, ¬S, ¬C respectively.
- **disk** — T and S are constant-true over the realizable space, so only C is a
  free condition ⟹ **MC/DC degenerates to 2 cases**: 1 honored (`disk∈C`) + 1
  reject (http-only client). The honored/reject pair flips C alone, holding
  `disk∈T`, `disk∈S` true — a valid independence pair.
- **http** — all three conditions are constant-true (Lemma H) ⟹ the predicate is
  constant ⟹ **1 honored case, no reject possible.**

**No-downgrade (exactness).** A hard request in `usable` must return *exactly*
that transport even when a higher-preference transport is also usable. Covered by
requesting `disk` and `http` while `cuda` is also usable (the image positives).

**Empty branch.** 1 defensive case (§3.2 corollary).

**Soft selection: branch coverage of the order walk.** Three outcome branches:
(a) the first in-order element is usable → returned; (b) an in-order element is
skipped (unusable) and a later one returned; (c) **no** in-order element is
usable → fallback to `usable[0]`.

**Discovery / readiness / liveness:** equivalence-class / decision-table —
one case per class or component (type axis: one per output type class;
readiness: ready vs ≥1-missing; liveness: wire-compat, WS handshake).

### 3.4 The 10 hard-selection cases, by basis

| # | case (id) | transport | formal basis |
|---|---|---|---|
| 1 | `image-cuda-usable` | cuda | MC/DC all-true (image·GPU·local) |
| 2 | `nonimage-cuda-rejected-by-type` | cuda | MC/DC ¬T isolated |
| 3 | `image-cuda-rejected-by-server` | cuda | MC/DC ¬S isolated |
| 4 | `image-cuda-rejected-by-client` | cuda | MC/DC ¬C isolated |
| 5 | `image-disk-usable` | disk | honored + no-downgrade (cuda also usable) |
| 6 | `nonimage-disk-usable` | disk | type equivalence-class representative |
| 7 | `nonimage-disk-rejected-by-client` | disk | MC/DC ¬C (disk's only free condition) |
| 8 | `image-http-usable` | http | constant-true + no-downgrade |
| 9 | `nonimage-http-usable` | http | type equivalence-class representative |
| 10 | `empty-intersection-fails` | — | defensive: empty branch (non-realizable input) |

So **5 honored + 4 axis-isolated rejects + 1 defensive = 10**. The "MC/DC"
property holds for **cuda's predicate** (rows 1–4); disk is the degenerate
single-free-condition pair (5/6 honored, 7 reject); http is the constant
predicate (8/9); row 10 is out-of-band robustness, not axis coverage.

### 3.5 Soft coverage — and a known gap

The canonical order is `cuda > http > disk`. The three soft cases:

| case | usable | outcome | branch |
|---|---|---|---|
| `soft-prefers-cuda-when-all-usable` | `{cuda,disk,http}` | cuda | (a) 0 skips |
| `soft-prefers-http-over-disk` | `{disk,http}` | http | (a) 1 skip (also: http ranks above disk) |
| `soft-debug-mask-forces-disk` | `{disk}` | disk | (a) 2 skips |
| `soft-fallback-when-no-order-match` | `{disk,http}` | disk | (c) fallback to `usable[0]` |

All three branches are covered: branch (a) by the first three cases (0/1/2
skips), and **branch (c)** — the no-match fallback — by
`soft-fallback-when-no-order-match` (order `[cuda]`, usable `{disk,http}` → no
order element usable → fallback to `usable[0]` = `disk`). Branch (b) ("skip an
unusable element, take a later one") is exercised within the (a) cases that skip
≥1 element. Soft selection = **4 cases**.

### 3.6 Why not the full Cartesian product (for selection)

`SelectOutputTransport` is a pure function; running every `(T,S,C,request)`
combination exercises the same branches as the boundary set above and proves
nothing more. So selection stays at the MC/DC-minimal set — minimality is
*correct* here, not a compromise.

**This reasoning does not transfer to delivery.** The delivery layer (§4 Layer 2)
is real I/O: each `(type × transport × topology)` triple is distinct machinery
(a real codec/file/GPU-buffer over a real filesystem or socket), so additional
triples add real coverage and minimality is *not* justified. Layer 2 is
**currently** a minimal boundary set (one positive per reachable transport per
topology + the reachability rejects); broadening it toward the full realizable
`(type × transport × topology)` matrix — every triple a real round-trip with
byte verification — is the intended direction and is the right place to spend
extra cases, since CI cost there is dominated by container/server startup, not
per-case time.

## 4. Exact case counts

### Layer 1 — Selection + Discovery (pure; single server; **no execution**) = 20

| Component | Axes | Count | Derivation |
|---|---|---|---|
| Setup / liveness | A0 | 2 | wire-compat + WS handshake |
| Type-axis discovery (live `/notch/parse`) | A1 | 2 | `|T|` = {image, non-image} |
| Deployment-readiness decision (live `/notch/get-required-files`; no execution) | A5 | 2 | all required files exist vs ≥1 missing |
| Hard selection | A1·A2·A3·A4 | 10 | 5 positives + 5 rejects (below) |
| Soft selection | A4 | 4 | canonical order `cuda>http>disk`: top-usable chosen (a/0-skip), http-over-disk when no cuda (a/1-skip), debug-mask forcing disk (a/2-skip), and the no-order-match fallback (c) |

Hard = **10**, derived in §3.4: **5 honored** (cuda image; disk image+non-image;
http image+non-image) + **4 axis-isolated rejects** (cuda by T, by S, by C; disk
by C) + **1 defensive empty-branch** case. The 4 cuda/disk rejects are the MC/DC
independence cases (each flips exactly one condition of `honored(x)`); the empty
case is **not** an axis reject — by Lemma H (§3.2) `usable` is never empty for a
realizable input, so it exercises the helper's empty branch on a non-realizable
input. http has no reject (it is never excludable).

### Layer 2 — Delivery coherence matrix

Layer 2 proves the **transports actually deliver bytes**, and that the
negotiation and the delivery agree: *the set the negotiation declares usable in a
topology is exactly the set that delivers; the set it rejects is exactly the set
that cannot.* This is the coherence the pure selection layer cannot prove — you
must not be able to negotiate to something you can't deliver.

The round-trip per case:

```text
NotchSingleInput fixture value
  -> /notch/inject execute=true   (after the client computes the real negotiation)
  -> workflow executes
  -> NotchOutputNode delivers through the negotiated transport
  -> mock client receives/fetches/imports the output
  -> byte/shape verification matches the injected fixture
```

Unlike selection (a pure function, MC/DC-minimal — §3.6), delivery is real I/O:
each `(output-type × transport × topology)` triple is **distinct machinery**, so
the matrix is **exhaustive over the realizable triples**, not minimal. CI cost is
dominated by container/server startup, not per-case time, so breadth here is
nearly free.

**Generator inputs (edit these; cases follow):**

- **Output types (7):** `image`, `audio`, `video`, `file_3d`, `mesh`,
  `load3d_camera`, `file_path` (`core/data_types.py` `_NOTCH_OUTPUT_TYPES`).
- **Input transport per type** (how the value reaches Comfy — fidelity matters):
  - **multipart upload (bytes really travel client→server):** `image`, `audio`,
    `video`, `file_3d`, `mesh` — the client uploads the source bytes, so the round
    trip exercises real input transport, not a server-local shortcut.
  - **inline value:** `load3d_camera` (a JSON camera dict in the inject body).
  - **server-staged path:** `file_path` — a path string to a file already on the
    server. This is **"server file-path copy"** coverage; it does NOT prove
    client-uploaded bytes (nothing leaves the client) unless a generic
    upload-to-temp mechanism is added.
- **Verification class per type** (how the delivered output is checked):
  - **byte-exact (SHA-256):** `file_path` only — the output is an exact `copy2`
    of the staged server file, so fetched/copied bytes equal the source.
  - **structural:** `file_3d`, `mesh`, `load3d_camera` — these reserialize
    (`File3D.save_to` re-emits the container, `save_glb_from_mesh` writes a GLB,
    `save_json_file` writes JSON), so delivered bytes legitimately differ from the
    upload — verify structurally (valid/parseable container + coherent shape), NOT
    an exact source hash.
  - **decoded (dims + dtype + sample hash):** `image`, `audio`, `video` —
    codec-transcoded; verify decoded shape/type and a deterministic sample.
- **Transports:** `cuda` (image-only, host-local only), `disk`, `http`.
- **Topologies → real client-reachable set:** `local` `{cuda,disk,http}`;
  `remote-http` `{http}`; `remote-route-disk` `{disk,http}`.

**Per topology, per type:** deliver every transport in `usable(type,topology)`
(positive, byte-verified) and reject every type-allowed transport *not* in
`usable` (negative, rejected before inject); then assert
`delivered == usable` and `rejected == type-allowed − usable`.

| Topology | usable | positives | coherence rejects | subtotal |
|---|---|---|---|---|
| `local` | image `{cuda,disk,http}`, non-image `{disk,http}` | image×3 + 6 non-image×2 = **15** | cuda-by-type, one per non-image type = **6** | 21 |
| `remote-http` | `{http}` | 7 types × http = **7** | disk-unreachable ×7 + cuda-unreachable (image) ×1 = **8** | 15 |
| `remote-route-disk` | `{disk,http}` (named route) | 7 types × {disk,http} = **14** | cuda-unreachable (image) = **1** | 15 |
| file-availability gate (runtime) | — | — | missing input file ⇒ run blocked = **1** | 1 |

**Total = 52 enumerated delivery cases.**

**Automatic generation — the arithmetic *is* the case set.** Cases are never
hand-written. The generator iterates `topology × type × transport` over
Σ={cuda,disk,http} and classifies each triple by the selection arithmetic, so the
counts above are a consequence of the loop, not a hand-count:

```text
for topology in [local, remote-http, remote-route-disk]:
  for type in the 7 output types:
    for X in {cuda, disk, http}:
      allowed   = X in type-allowed(type)         # cuda iff image
      server_ok = X in server-available(topology) # GPU runner -> {cuda,disk,http}
      client_ok = X in client-reachable(topology) # local{c,d,h} http-only{h} route-disk{d,h}
      usable    = allowed and server_ok and client_ok
      if usable:           emit POSITIVE(topology, type, X)
      elif not allowed:    emit REJECT(topology, type, X, axis=type)    # only in `local`
      elif not server_ok:  emit REJECT(topology, type, X, axis=server)
      elif not client_ok:  emit REJECT(topology, type, X, axis=client)
```

The single de-dup rule — **type-axis rejects are emitted only in `local`** — turns
the raw 21-per-topology product into the curated 22/30: a transport excluded by
*type* (cuda for a non-image) rejects identically in every topology, so it is
proved once; rejects by *server* or *client* are topology-specific and emitted per
topology. This yields exactly `local` 15 + 6 = 21 (+ file-availability gate = 22),
`remote-http` 7 + 8 = 15, `remote-route-disk` 14 + 1 = 15 → remote total 30.

**Self-describing case id (derived, never hand-named):**

```text
<topology>.<type>.<transport>.<verdict>      verdict in {deliver, reject-type, reject-server, reject-client}
```

The id and the expectation come from the same arithmetic, so they cannot disagree:

| id | the expectation it encodes |
|---|---|
| `local.image.cuda.deliver` | cuda usable for image locally → inject (multipart), execute, deliver over cuda, verify cuda-raw |
| `local.audio.cuda.reject-type` | cuda not type-allowed for audio → `SelectOutputTransport` refuses before inject (no downgrade) |
| `remote-http.image.disk.reject-client` | disk type-allowed + server-available but client can't reach the filesystem → refused before inject |
| `remote-route-disk.image.cuda.reject-client` | cuda is host-local-only, unreachable from the route-disk client → refused before inject |

`POSITIVE` expects: client negotiates X (must be usable), inject per the type's
input transport, execute, deliver over X, verify per the type's verification class.
`REJECT` expects: the client refuses X before inject and names the excluding axis;
no request reaches the server. The per-topology coherence assertion
(`delivered == usable`, `rejected == type-allowed − usable`, plus the type-reject
set) is then a check over the generated ids, not a separate hand list.

**Capability gating (skips are self-clearing, never silent):**

- **CUDA** cases (the `local` image-cuda positive, plus the CUDA reader path) skip
  with reason `cuda_unavailable` when the runner has no CUDA device — exactly as
  §2 gates cuda on `cuda_device_index ≥ 0`. A second, narrower skip is allowed:
  `cuda_ipc_unsupported` when a WSL-marked runner produced a valid CUDA share
  contract (execution success, share-status metadata, `/notch/cuda/share`
  metadata, WS/HTTP handle integrity, and matching device index) but raw legacy
  IPC import fails. That skip must be accompanied by the CUDA IPC evidence
  artifacts (`cuda-ipc-probe.json`, `cuda-simple-ipc-result.json`,
  `cuda-ipc-ld-debug.log`, `libcuda-ldconfig.txt`). On non-WSL runners, or when
  the share contract is not valid, an available-but-failed import is a **fail**.
- **`remote-route-disk`** is active when the action config provides a matching
  server/client named route and `/features` advertises that route under
  `extension.notch.named_disk_routes.route_ids`. The route ID is deployment
  capability data, not the selected output transport. A disk request still
  carries per-run `config.output.disk.named_route_id` plus optional
  `relative_directory`, and the ready event returns the same `named_route_id`
  plus final `relative_path`.

**Topologies as jobs:**

- **delivery-local** — single container; mock client and ComfyUI share one
  filesystem and host-local CUDA IPC. Covers the `local` row (21) plus the
  file-availability runtime gate (1) = 22.
- **delivery-remote** — two containers on a private Docker network. Without a
  named route it covers the `remote-http` reachability regime. With a host-backed
  route directory mounted at *different* paths per container (e.g.
  `/srv/notch-named-routes/ci_shared_output` server,
  `/mnt/notch-named-routes/ci_shared_output` client), it covers the
  `remote-route-disk` regime: the route config (`named_route_id → server_root` /
  `named_route_id → client_root`) is the agreement, the request carries
  `named_route_id` not an absolute path, and the ready event's `relative_path`
  plus client route root is the client-readable artifact path.

Both delivery jobs depend on `extension-boot` and `protocol-negotiation`. There
is no standalone run-lifecycle job: a workflow that only proves
`/notch/inject?execute=true` reaches a terminal event has no `NotchSingleInput`,
no `NotchOutputNode`, no transport, and no byte verification, so it proves
nothing delivery does. Successful execution is proved *inside* each positive
delivery case by the prompt-matched WebSocket terminal event.

Layer 2 excludes output feature/generation policies (metadata embedding, manifest
embedding, previews) — out of scope per §Scope.

**Implementation status.** The matrix above is the **target**. Implemented today:
7 of 52 — `local` disk/http/cuda (3 positives), `remote-http` http positive (1),
the `remote-http` disk/cuda reachability rejects (2), and `remote-route-disk`
file-path disk positive (1). The remaining positives (per-type breadth) and the
per-topology coherence rejects are not yet generated. The
`delivery-local`/`delivery-remote` cases are not stub-verifiable (they need a
live server); only the negotiation layer and pure cardinality arithmetic are
covered by `stub_check`.

### Totals (exact)

```
Layer 1 (selection/discovery/readiness) — fully implemented:
  2 liveness + 2 type-axis + 2 readiness + 10 hard + 4 soft = 20  (all active)

Layer 2 (delivery coherence matrix) — target 52, implemented 7:
  target    = 52  (local 21 + file-availability 1 + remote-http 15 + remote-route-disk 15)
  implemented = 7  (local disk/http/cuda + remote-http http positive + 2 remote rejects
                    + remote-route-disk file_path disk)
  capability-skipped at target: cuda cases skip without a GPU

full conformance: Layer 1 = 20 implemented; Layer 2 = 7 of 52 implemented
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
| hard/soft selection | `usable = T∩S∩C`; client returns not-usable (no downgrade) | C++ `SelectOutputTransport` (client-side decision); expected sets from `OUTPUT_TYPES_BY_TRANSPORT`, `supported_output_transports()` | pure helper (server-grounded expectations) |
| server hard-error (delivery) | an impossible requested transport is **rejected at inject**, not downgraded | `build_notch_output_config` / `assert_transport_available` in `services/output_config_service.py`, `services/server_capabilities.py` | live (delivery only) |
| delivery round-trip | actual disk/http/cuda delivery + events + byte verification | `services.output_delivery`; `services.cuda_shares`; `notch-output-ready`; `notch-cuda-share-status` | live |

"pure helper" means the case exercises the **C++ interface** logic; its *expected
values* come from extension source. Delivery cases repeat the same calculation
with live server facts and topology-specific client reachability before
execution.

## 7. Generation — spec to tests

The cases are generated from data, not hand-written per test:

- **Selection** — `mock_client/src/matrix.cpp` `kHardRows` (10) and `kSoftRows`
  (4). Each row is one `(typeAllowed, serverAvailable, clientReachable,
  requiredOrPreference, expect)` tuple → one case. Add a row = add a case. Hard
  rows set a single required transport; soft rows pass a preference order — the
  canonical `cuda>http>disk` for the first three, the debug-mask narrowing
  `clientReachable` to force disk, and `[cuda]` for the fallback case (see
  §Negotiation in ComfyUI-Notch `features.md`).
- **Type-axis** — one case per output in the `/notch/parse` fixture; expected
  transports come from `ExpectedTypeTransports()` (image → cuda,disk,http; else →
  disk,http).
- **Readiness** — one case per required-files fixture (`requiredFilesReadyJson`,
  `requiredFilesMissingJson`); gate = "any `exists=false` ⇒ blocked".
- **Setup/WS** — fixed.

`mock_client/tests/stub_check.cpp` runs the **Layer 1** (negotiation) matrix
against stub transports (no server, no IXWebSocket, no network) and asserts the
total. **It is the executable form of §4 Layer 1** — change the selection/discovery
spec, change the asserted count. Layer 2 (delivery) is *not* stub-verifiable (it
needs a live server), so it is not covered here. Today:

```
stub conformance: pass=20 fail=0 skip=0 error=0   ← Layer 1 exact
```

### Artifact shape

Each case writes `conformance/cases/NNN-<id>/case.json`, **prettified** and
**self-describing**, so the evidence reads without the source:

- `title` — plain one-line summary of what the case checks.
- `phase` — `liveness` | `discovery` | `readiness` | `transport-selection` |
  `delivery-local` | `delivery-remote`.
- `description` — why the case exists, in prose.
- `spec_ref` — pointer back into this document.
- the request: `required_transport` (a single required transport, never
  downgraded) or `preferred_order` (first usable wins) for selection cases.
- `expected` vs `actual`, `result`, `errors`.

The run also writes `conformance/INDEX.md` — every case grouped by phase
with title + result — as the human entry point. The `.json` files are
pretty-printed; the run-wide `.jsonl` streaming logs (`mock-client.jsonl`,
`http.jsonl`, `websocket.jsonl`) stay one record per line (the JSON Lines
contract — read line by line, not pretty-printed).

## 8. Status

The runnable public jobs are:

```text
extension-boot -> protocol-negotiation -> delivery-local -> delivery-remote
```

`extension-boot` proves the extension loads and the C++ client interface
compiles. `protocol-negotiation` proves Layer 1: discovery, readiness decision,
transport selection, and the WebSocket handshake.

`delivery-local` and `delivery-remote` prove the actual Notch round trip:
inject an input value, execute, receive the transport-specific Notch signal,
retrieve/import the output, and verify delivery evidence. Each delivery case
also records its negotiation inputs and choice.

Do not add a permanent standalone execution job. Delivery owns execution
lifecycle assertions.

**Implemented today:** Layer 1 = **20** (fully implemented, stub-verified);
Layer 2 = **7 of the 52-case target** delivery matrix (§4): `local`
disk/http/cuda positives, the `remote-http` http positive, the two
`remote-http` reachability rejects, and the `remote-route-disk` file-path disk
positive. The remaining delivery breadth (per-type positives and per-topology
coherence rejects) is the target in §4, not yet generated. Active counts are
environment-dependent: cuda delivery skips without a GPU.

## 9. Delivery Diagnostics — WS assertions and per-case Notch diagnostics

Delivery cases execute workflows, so each case gains two diagnostic channels.
Both are **structured, never regex over the server log** — the ComfyUI log format
is not a stable contract, so the matrix must not assert on parsed log text. Raw
log is kept only as a per-case byte-offset slice (`server.log`) for human
context. **The verdict is the contract behavior only:**

```text
WS terminal event + transport-specific Notch signal + byte/shape validation
```

Diagnostics (§9b) are **attached evidence, not a verdict input** — they help
explain a failure but never cause one.

### 9a. WebSocket lifecycle assertions (client-side, no new server work)

The mock client holds the `/ws` connection and forwards every frame into the
facade (`Client::OnWebSocketText`), which parses lifecycle events keyed by
`prompt_id`, correlates prompt → consumer, and dispatches typed `ClientEvent`s
(`EventKind::ExecutionSuccess` / `ExecutionError` / `ExecutionInterrupted`) to the
`ClientEventSink`. Per execution case:

1. Submit via `Client::Submit()` (`/notch/inject`); read `prompt_id` from the
   returned `JobHandle`.
2. Wait for the matching terminal event on `/ws`.
3. Assert it: `execution_success` for positive cases; `execution_error` (with
   `exception_type`, `node_id`, `traceback`) for cases that must fail at runtime.
4. Record raw frames to `websocket.jsonl`, the parsed terminal/output state to
   `websocket-summary.json`, and the terminal outcome to `case.json`.

Pre-queue validation failures arrive as structured JSON in the inject HTTP
response (`error`, `node_errors`) — also no log parsing.

### 9b. Per-case Notch diagnostics

`NOTCH_CI` is distinct from `NOTCH_DEBUG`: it should not turn on noisy stdout
debug logging. Instead, ComfyUI-Notch exposes structured prompt-scoped records
through `GET /notch/diagnostics?prompt_id=…`:

**Consumption rule: diagnostics are context, not pass/fail.** A case's verdict
comes solely from its expected outcome (selection / gate / WS terminal event +
byte verification). Diagnostics are recorded as evidence and never fail a case on
their own — this matches the harness (`missingDiagnostics` is written to
`case.json` but is never pushed to `errors` and never gates the result).

- `kind: "log"` records are normal `WARNING`+ log records — pure context (e.g. a
  cache miss, a proceed-without-X path). A case can pass with warnings attached.
- `kind: "event"` records are explicit CI-only decision breadcrumbs emitted along
  the inject/execution/output path. They are recorded per case (the
  `diagnostics_missing_events` field lists any expected-but-absent events as
  **evidence**), but a missing breadcrumb does **not** fail the case. (A missing
  breadcrumb on an otherwise-passing case may be surfaced as a warning annotation
  for visibility; the verdict is unchanged.)

The expected positive-case breadcrumb path (recorded, not gating) is compact:

```text
inject.request_parsed
inject.output_config_patched
workflow.inputs_resolve_start
workflow.inputs_stamped
inject.inputs_stamped
inject.queue_submit
inject.queue_response
single_input.resolve_value
single_input.parse_value
output_node.config_resolved
output_node.payload_resolved
output_node.delivery_start
output.<transport>_delivery_start
output.<transport>_delivery_done
output.ready_broadcast        (disk/http)
cuda.share_status_broadcast   (cuda)
output.cuda_delivery_done     (cuda)
output_node.delivery_done
```

For multipart/raw-buffer cases, `input.multipart_processed` is also expected. The
matrix writes the full response to `notch-diagnostics.json` for every delivery
case as attached evidence.
