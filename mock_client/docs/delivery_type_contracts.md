# Delivery Type Contracts (Plan A, Task 1 findings)

Source-grounded contract for each of the 7 Notch output types — what to inject,
how the value is parsed, what `NotchOutputNode` writes, and therefore how the
delivered artifact must be verified. Traced through ComfyUI-Notch source:
`node_impl/single_input.py`, `services/execution_value_parser.py`,
`node_impl/output_node.py`, `services/output_delivery.py`, `core/data_types.py`.

## Round trip

`/notch/inject` writes the value into the prompt's `remote_value`. `NotchSingleInput`
reads it and parses via `execution_value_parser.get_handler_for_type(type)`. The
parsed payload flows to the wired `NotchOutputNode` slot, which delivers through
`services/output_delivery.py` (`DiskOutputDelivery` for disk+http, `CudaOutputDelivery`
for cuda).

## Per-type table (confirmed)

| output type | inject value (`remote_value`) | NotchSingleInput type | parser → payload | NotchOutputNode slot | delivery writes | verification |
|---|---|---|---|---|---|---|
| `file_path` | **path string** | `STRING` | `_parse_string` → `str` | `file_path` | `shutil.copy2(src, dest)` — exact byte copy | **byte-exact (SHA-256)** |
| `file_3d` | **path string** | `FILE_3D` | `_parse_file_3d` → `LoaderRegistry.load(FILE_3D)` → `File3DValue` | `file_3d` | `file_3d.save_to(path)` — **re-serialized by the payload**, not a raw copy | **byte-exact ONLY IF `File3DValue.save_to` is a byte pass-through — UNCONFIRMED; otherwise integrity/structural** |
| `image` | path string (disk/http); raw f32 buffer (cuda) | `IMAGE` | `_parse_image` → `LoaderRegistry.load(IMAGE)` → tensor | `image` | disk/http: `PIL.save(format=ext)` — **re-encoded** (transcoded); cuda: raw f32 RGBA share | **decoded-image (disk/http, stb_image); cuda-raw (cuda)** |
| `audio` | path string | `AUDIO` | `_parse_audio` → `LoaderRegistry.load(AUDIO)` → `{waveform, sample_rate}` | `audio` | `write_audio_file` — WAV/PyAV **re-encode** (transcoded) | **integrity** |
| `video` | path string | `VIDEO` | `_parse_video` → `VideoValue` | `video` | `save_video_file` — mp4 **re-encode** (transcoded) | **integrity** |
| `mesh` | **path string** | `MESH` | `_parse_mesh` → `file3d_to_mesh(load(FILE_3D))` → `MeshValue(vertices,faces)` | `mesh` | `runtime.save_glb_from_mesh(payload, prefix)` — **writes a GLB binary**, NOT JSON | **structural over GLB (NOT JSON) — see decision below** |
| `load3d_camera` | **inline dict** (camera params) | `LOAD3D_CAMERA` | `_parse_load3d_camera` → `default_camera_info().update(val)` → `dict` | `load3d_camera` | `save_json_file` → `json.dump(indent=2)` — **JSON** | **structural JSON** |

## Corrections to `notch_conformance_spec.md` §4 (the spike's payoff)

1. **`mesh` is NOT "structural JSON".** `_deliver_mesh` calls `runtime.save_glb_from_mesh`
   → a **GLB binary** (12-byte header + JSON chunk + BIN chunk), not a JSON document.
   Only `load3d_camera` is JSON (`save_json_file`). The spec's "structural: mesh,
   load3d_camera (JSON)" conflates them. → dispatch a `doc-drift-fixer` for the spec.

2. **`file_3d` is NOT guaranteed byte-exact.** It round-trips
   path → `LoaderRegistry.load(FILE_3D)` → `File3DValue.save_to(dest)`. The delivered
   bytes equal the source ONLY IF `save_to` re-emits the original bytes unchanged.
   `resolve_output_extension` uses `file3d_format(payload)` (the payload's own format),
   which suggests a re-serialization path. **Must read `File3DValue` /
   `file3d_format` / `save_to` in `comfy_adapter/types.py` before choosing byte-exact.**
   If `save_to` is not a pass-through, file_3d is **integrity** (valid container +
   non-empty), not byte-exact.

3. **`load3d_camera` fixture is an inline dict, not a file.** The injected value is a
   dict merged into `default_camera_info()`. Expected output = `default_camera_info()`
   deep-merged with the injected dict, JSON-dumped. The mock client must compute the
   expected merged dict (or fetch `default_camera_info` shape) to compare structurally.

## Verification-class decisions (feeding Tasks 2–7)

- **byte-exact** — `file_path` (confirmed `shutil.copy2`). `file_3d` *pending* the
  `save_to` check.
- **decoded-image** — `image` disk/http via stb_image (dims+channels+sample hash);
  tolerates PIL re-encode.
- **cuda-raw** — `image` cuda (existing path; raw f32 RGBA vs expected pattern).
- **integrity** — `audio`, `video` (re-encoded; verify the WS `notch-output-ready`
  fired + artifact exists + non-empty + server-reported metadata is coherent).
- **structural JSON** — `load3d_camera` (canonical-JSON compare of delivered vs the
  expected merged camera dict).
- **mesh** — OPEN DECISION (see below).

## OPEN DECISION: how to verify `mesh`

The output is a GLB written from a `MeshValue` that itself came from
`file3d_to_mesh(load(source.glb))`. The load→mesh→glb path is lossy/non-deterministic
(re-tessellation, accessor repacking), so byte-equality and exact geometry-equality
are both fragile. Options:

- **(a) GLB-validity + integrity (recommended):** assert the delivered file is a valid
  GLB — magic `0x46546C67`, version 2, a parseable JSON chunk (use the bundled
  `jsonxx`), a non-empty BIN chunk, and ≥1 mesh/accessor declared in the JSON chunk.
  No glTF geometry decoder needed; proves a real mesh was delivered. Cheap, robust.
- **(b) Full geometry compare:** vendor a glTF/GLB parser (e.g. `cgltf.h`), decode
  vertices/faces, compare counts/bounds to the input mesh. Heavy dep, fragile across
  the lossy round trip.
- **(c) Re-inject proxy:** feed the delivered GLB back through a second
  `file_3d`/`mesh` inject and compare structurally. Doubles the round trip per case.

Recommendation: **(a)** — matches the integrity philosophy used for audio/video,
needs only the already-vendored `jsonxx`, and is stable across the lossy mesh path.
