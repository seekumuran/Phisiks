# PAI: the PHISIKS Agent Interface

PAI lets an external program (an AI agent's tool layer, a script, a test harness) inspect and edit the
scene open in the PHISIKS editor, and run its simulation, through a small documented protocol.

**PHISIKS contains no AI and needs no network or cloud service.** The client is a separate process
that the user starts. PAI is off unless the editor is started with `--pai`.

| | |
|---|---|
| Protocol | JSON-RPC 2.0, one message per line (UTF-8, newline-delimited) |
| Protocol id / version | `phisiks-pai` / `1.0` (returned by `initialize`) |
| Transport | the editor process's standard input and output. There is no network listener. |
| Library | `PhisiksPAI` (`apps/pai/src`), linked into the `Phisiks` editor; SDK target `Phisiks::PAI` |
| Example client | `examples/pai_client/pai_client.py` (standard-library Python) |
| Tests | ctest `PaiProtocol` (`pai_client.py --check`), also run in Linux CI on llvmpipe |

## 1. Architecture

```
 agent / tool process                       Phisiks editor process
 ───────────────────                        ──────────────────────────────────────────────
  spawns the editor  ── stdin (requests) ──▶ reader thread ─▶ bounded queue (64)
                                                                  │
                     ◀─ stdout (responses) ─ frame hook (main thread, ≤ 32 / frame)
                                                 │
                                                 ▼
                                            Pai::Dispatcher ── validates, checks permission
                                                 │
                                   Editor public API  (BeginEdit / EndEdit, Undo, EnterPlay,
                                   StopPlay, AssignAsset, SaveScene, ProjectAssets)
                                                 │
                                   SceneSerializer (the same codecs that load scene files)
```

- Requests are read on a background thread but **handled on the editor's main thread**, at the start
  of an editor frame, where the scene lives. No scene state is touched from another thread.
- PAI uses only the editor's public API (`apps/editor/src/Editor.h`) and the engine's public
  headers, the same interfaces an SDK consumer uses. The layering check (`tools/check_layering.cmake`)
  enforces this: PAI sources may include public headers, `Editor.h` and the private nlohmann/json
  header only.
- Each mutation is wrapped in `Editor::BeginEdit` / `EndEdit`, so it is **one undo step** labelled
  `PAI: …`, shown in Edit > Undo like any edit made with the mouse.
- `Pai::Dispatcher` is transport-independent. An in-process tool can construct one and call
  `Handle(ctx, requestText)` from an `Editor::AddFrameHook` callback.
- While PAI runs, the engine's console log goes to **stderr**, so stdout carries only responses.
  The log file is unchanged.

## 2. Starting a session

```
Phisiks --pai [--pai-write] [--pai-simulate] [--hidden] [--open scene.json] [--project dir]
```

PAI needs the editor, so `--pai` cannot be combined with `--no-ui` or `--headless`; use `--hidden`
for no window. When the client closes stdin, the editor keeps running for the user; the client
decides whether to end the process.

The intended pattern is that **the client starts the editor** (as `pai_client.py` does). The
pipes then belong to that one process pair and nothing else on the machine can connect.

## 3. Permissions

Permissions are chosen by the user on the command line and fixed for the session. A client cannot
raise them.

| Permission | Flag | Methods |
|---|---|---|
| `read` | `--pai` | `initialize`, `pai.describe`, `project.info`, `engine.state`, `scene.components`, `scene.list`, `scene.get`, `assets.types`, `assets.list`, `assets.get`, `assets.missing`, `history.get`, `sim.status`, `sim.metrics`, `cad.get` |
| `write` | `--pai-write` | `scene.create`, `scene.delete`, `scene.rename`, `scene.set_transform`, `scene.set_component`, `scene.remove_component`, `scene.set_parent`, `assets.assign`, `history.undo`, `history.redo`, `scene.save`, `cad.create`, `cad.set_document`, `cad.set_parameter`, `cad.export` |
| `simulate` | `--pai-simulate` | `sim.play`, `sim.pause`, `sim.resume`, `sim.step`, `sim.stop` |

`--pai-write` and `--pai-simulate` imply `--pai`. `initialize` lists only the methods granted;
`pai.describe` lists every method with `"granted": true | false`.

## 4. Messages

A request is an object with `"jsonrpc": "2.0"`, a string `"method"`, optional named `"params"`
(an object; positional arrays are refused) and an `"id"` (string, integer or null). Without an
`id` it is a notification and gets no response. A batch is an array of up to 16 requests; the
response is an array of the responses, in order.

Entities are addressed by `id`, the entity's stable 16-hex-digit GUID (saved in scene files), or
by `name` (the first entity with that name). Assets are addressed by their asset id.

### Errors

| Code | Meaning |
|---|---|
| -32700 | parse error (not JSON; also `NaN` / `Infinity`, which JSON does not have) |
| -32600 | invalid request (shape, or larger than `maxRequestBytes`) |
| -32601 | method not found |
| -32602 | invalid params (missing, wrong type, out of range, a component value the serializer rejects, a hierarchy cycle) |
| -32603 | internal error |
| -32001 | permission denied (permission not granted, or an unsafe value: file path, script) |
| -32002 | not possible now (mode or state: editing while playing, nothing to undo, no scene file) |
| -32003 | not found (entity, asset, component) |
| -32004 | limit exceeded (queue full, batch too large, too many frames, name too long) |

## 5. Methods

`pai.describe` returns this list from the running editor, with parameters and descriptions; it is
the authoritative manifest.

**Discovery and inspection (read)**

| Method | Params | Result |
|---|---|---|
| `initialize` | `{ client? }` | protocol, versions, transport, permissions, limits, granted methods |
| `pai.describe` | `{}` | the full manifest (every method, permission, params, description, error codes) |
| `project.info` | `{}` | project folder, scene name and file, `edit`/`play` mode, unsaved changes, undo/redo counts and labels |
| `engine.state` | `{}` | engine version, run state, frame, simulated time, fixed steps, CPU frame ms, render statistics |
| `scene.components` | `{}` | every component key (`transform`, `meshRenderer`, `rigidbody`, `collider`, `light`, `camera`, `joint`, …) |
| `scene.list` | `{ name?, component?, offset?, limit? }` | entities sorted by id: id, name, parent, child count, component keys |
| `scene.get` | `{ id \| name }` | local transform, world position, every component with its values (scene-file JSON), materials, child ids |
| `assets.types` | `{}` | asset types and file extensions of the project asset pipeline |
| `assets.list` | `{ type?, folder?, offset?, limit? }` | assets of the open project: id, type, path, size, hash, revision, missing flag, reference string |
| `assets.get` | `{ id }` | one asset, plus how many scene references use it |
| `assets.missing` | `{}` | scene references that do not resolve |
| `history.get` | `{}` | can undo / redo, labels, counts, memory used |
| `sim.status` | `{}` | mode, state, simulated time, fixed steps, frames left of a `sim.step` |
| `sim.metrics` | `{ ids? }` | world position, rotation and linear velocity (default: every rigid body) |
| `cad.get` | `{ id \| name, document? }` | a CAD part: kernel, unit, parameters, features (built / skipped / error, sketch degrees of freedom), bodies, volume, area, bounds, face / edge counts, validity; with `document: true` also the parametric document |

**Editing (write)**: every call is one undo step and is refused while playing.

| Method | Params | |
|---|---|---|
| `scene.create` | `{ name, parent?, position?, rotationDegrees?, scale?, components? }` | `components` maps keys to values in scene-file form |
| `scene.delete` | `{ id \| name }` | deletes the entity and its children |
| `scene.rename` | `{ id \| name, newName }` | |
| `scene.set_transform` | `{ id \| name, position?, rotationDegrees? \| rotation? ([w,x,y,z]), scale? }` | local transform |
| `scene.set_component` | `{ id \| name, component, value }` | adds the component or patches the given fields; other fields keep their values |
| `scene.remove_component` | `{ id \| name, component }` | |
| `scene.set_parent` | `{ id \| name, parent (id or null), keepWorld? }` | cycles are refused |
| `assets.assign` | `{ entity, asset }` | assigns a mesh, model, texture or audio asset the way drag-and-drop does |
| `history.undo` / `history.redo` | `{}` | the editor's own history (includes the user's edits) |
| `scene.save` | `{}` | saves the open scene **to its own file only**; refuses to drop components that can't be stored |
| `cad.create` | `{ name, document? }` | a new CAD part (docs/CAD.md); the document is validated |
| `cad.set_document` | `{ id \| name, document }` | replaces a part's parametric document; returns the rebuilt part's report |
| `cad.set_parameter` | `{ id \| name, parameter, expression }` | adds or changes a parameter; an invalid expression is refused and not kept |
| `cad.export` | `{ id \| name, format }` | `step`, `iges`, `stl`, `brep` or `phcad`, written **only** to `<project>/assets/exports/` |

**Simulation (simulate)**

| Method | Params | |
|---|---|---|
| `sim.play` | `{}` | enters play mode: the edit state is captured first |
| `sim.pause` / `sim.resume` | `{}` | |
| `sim.step` | `{ frames? (1–3600) }` | runs n editor frames then pauses; enters play mode if needed. Poll `sim.status` until `stepping` is 0 |
| `sim.stop` | `{}` | back to edit mode; everything changed while playing is discarded |

## 6. Examples

These exchanges were recorded from the editor (`Phisiks --pai --pai-write --pai-simulate --hidden`).

```jsonc
--> {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"client":"my-agent"}}
<-- {"id":1,"jsonrpc":"2.0","result":{"engine":"PHISIKS","engineVersion":"1.1.0","limits":{"maxListed":5000,"maxPerFrame":32,"maxQueued":64,"maxRequestBytes":262144,"maxStepFrames":3600},"methods":["assets.assign", … ,"sim.stop"],"permissions":{"read":true,"simulate":true,"write":true},"protocol":"phisiks-pai","protocolVersion":"1.0","transport":"stdio, newline-delimited JSON-RPC 2.0"}}

--> {"jsonrpc":"2.0","id":2,"method":"scene.list","params":{"limit":2}}
<-- {"id":2,"jsonrpc":"2.0","result":{"entities":[{"children":0,"components":["light"],"id":"2e398a2ccee928ab","name":"Sun","parent":null},{"children":0,"components":["camera"],"id":"534ac64e6b0ebc23","name":"MainCamera","parent":null}],"offset":0,"total":4}}

--> {"jsonrpc":"2.0","id":3,"method":"scene.create","params":{"name":"Box","position":[0,2,0],"components":{"meshRenderer":{"mesh":"Cube"},"rigidbody":{"mass":2}}}}
<-- {"id":3,"jsonrpc":"2.0","result":{"children":0,"components":["meshRenderer","rigidbody"],"id":"a868f36178919089","name":"Box","parent":null}}

--> {"jsonrpc":"2.0","id":4,"method":"scene.set_component","params":{"name":"Box","component":"meshRenderer","value":{"mesh":"C:/secrets.obj"}}}
<-- {"error":{"code":-32001,"message":"meshRenderer.mesh accepts a built-in mesh name or a project asset reference (asset:<id>/<name>), not a file path"},"id":4,"jsonrpc":"2.0"}

--> {"jsonrpc":"2.0","id":5,"method":"sim.step","params":{"frames":30}}
<-- {"id":5,"jsonrpc":"2.0","result":{"stepping":30}}
--> {"jsonrpc":"2.0","id":6,"method":"sim.status"}
<-- {"id":6,"jsonrpc":"2.0","result":{"fixedSteps":14,"mode":"play","simulationTime":0.2333,"state":"Paused","stepping":0}}

--> {"jsonrpc":"2.0","id":7,"method":"scene.rename","params":{"name":"Box","newName":"B"}}
<-- {"error":{"code":-32002,"message":"the simulation is playing: stop it first (sim.stop) to edit the document"},"id":7,"jsonrpc":"2.0"}

--> {"jsonrpc":"2.0","id":8,"method":"sim.stop"}
<-- {"id":8,"jsonrpc":"2.0","result":{"mode":"edit","status":"stopped: edit state restored"}}

--> {"jsonrpc":"2.0","id":9,"method":"history.undo"}
<-- {"id":9,"jsonrpc":"2.0","result":{"undone":"PAI: create 'Box'"}}
```

A read-only session (`--pai` alone):

```jsonc
--> {"jsonrpc":"2.0","id":1,"method":"scene.delete","params":{"name":"Sun"}}
<-- {"error":{"code":-32001,"message":"'scene.delete' needs the write permission (start the editor with --pai-write)"},"id":1,"jsonrpc":"2.0"}
```

Python:

```python
from pai_client import PaiClient
with PaiClient("Build/bin/Release/Phisiks.exe", write=True, simulate=True) as pai:
    box = pai.call("scene.create", name="Box", position=[0, 4, 0],
                   components={"meshRenderer": {"mesh": "Cube"}, "collider": {"shape": "Box"}, "rigidbody": {"mass": 2}})
    pai.call("sim.step", frames=60)
    # poll pai.call("sim.status")["stepping"] until 0, then:
    print(pai.call("sim.metrics", ids=[box["id"]]))
    pai.call("sim.stop")
```

## 7. Security model

What PAI enforces:

- **Off by default.** Only `--pai` on the command line starts it; nothing in a scene or project file can.
- **No network.** stdio only; there is no listener, no port and no authentication to get wrong.
- **Separated permissions.** Read, write and simulate are granted separately and checked on every call.
- **No file system access.** No method takes a file path. File-like component fields (`meshRenderer.mesh`,
  `foliageScatter.mesh`, `audioSource.clip`, `skinnedModel.path`, `terrain.heightmapImage`) accept
  only built-in mesh names or project asset references (`asset:<id>/<name>`), which resolve inside
  the project's `assets/` folder. `scene.save` writes only the file the open scene came from.
- **No code execution.** `script` components cannot be created or edited, and script assets cannot
  be assigned. There is no shell, eval or plugin-loading method.
- **Validated values.** Component values go through `SceneSerializer`, the code that loads scene
  files: unknown keys, wrong types and out-of-range values are refused. Vectors must be finite and
  within ±10⁷. Hierarchy cycles are refused.
- **Edit / play safety.** Mutations are refused while playing. `sim.stop` restores the edit state.
  Every mutation is undoable.
- **Bounded work.** Requests over 256 KiB are refused, the queue holds 64 requests (more are refused
  with -32004), at most 32 are handled per frame, lists return at most 5000 items, `sim.step` runs
  at most 3600 frames, batches hold at most 16 requests and names at most 256 characters. These are
  the `Pai::Limits` defaults; an embedding program can lower them.

Limitations (what PAI does **not** protect against):

- A client with `write` can change or delete anything in the open scene, and `scene.save` writes
  that to the scene's file. Undo is available until the editor closes. Grant `--pai-write` only
  to clients you trust with the document.
- Anyone who can start the editor with `--pai` already has the user's own account; PAI adds no
  privilege boundary beyond the editor itself, and it is not a sandbox for untrusted agents.
- A client can keep the editor busy within the limits above (for example, repeated `sim.step`, or
  CAD documents with many expensive features: each `cad.*` edit regenerates the part on the main
  thread). CAD documents are parsed by PHISIKS's validating reader; an `import` feature's embedded
  BREP text is parsed by Open CASCADE.
- There is no authentication, because the only peer is the process that owns the pipes.
- Other processes running as the same user can, as always, read the scene file or inspect the
  editor's memory; PAI does not change that.
- `sim.metrics` reports what the engine simulates. The physics is not validated for engineering use
  (see KNOWN_ISSUES.md).

## 8. Not exposed (by design or not implemented)

- File import, file open and Save As (arbitrary paths); project switching.
- Lua scripts and any form of code.
- Renderer and quality settings; screenshots or image export (use the CLI `--export-*` options).
- Interactive CAD sketching and picking (agents edit the parametric document instead: `cad.*`).
- A localhost / TCP transport. stdio covers local agents without opening a port; a network
  transport would need authentication and is not part of 1.1.
