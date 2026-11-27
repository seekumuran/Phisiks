# PHISIKS — Final Release Audit

**Audit date:** 2026-11-27 · **Target:** public release in about 48 hours · **Tree audited:** `main` @ `784cede` plus the uncommitted working tree (13 modified files, ~20 new files: Marina, Old-McDonald, Rover CAD, Drone CAD, ProjectKit, CadWorkspace).

**Test machine:** Windows 11 Home 26200, Intel Core i5-12450H (12 threads), Intel UHD Graphics (iGPU), Visual Studio 18 (2026) **Insiders**, CMake 4.5.0-rc1, PhysX + WASAPI backends.

**Labels.** Each finding has one of four statuses:
- **VERIFIED**: works, shown by running it.
- **BROKEN**: exists but misbehaves.
- **INCOMPLETE**: partly there.
- **MISSING**: not there at all.

Each finding also says how it was established:
- **[run]**: the real application or test suite, run during this audit.
- **[harness]**: a scratch program, outside the repo, linked against the real compiled Lab/engine objects. It makes the same calls the UI makes.
- **[code]**: reading the source only, not reproduced at runtime.

No repository files were changed during the audit apart from creating this document. All test output went to the session scratch directory.

---

## 1. Executive summary

The engine, renderer, test harness and the four engineering workspaces are in good shape. Everything builds cleanly with zero warnings. All automated tests pass on the default OpenGL 4.6 tier, both from the build tree and from the installed release layout. On the 4.1 (low-end) tier, one check fails: the Old-McDonald mission hits the test's frame cap (H8). The workspaces load, render, pick, explode, save and reopen through their own panel buttons.

The release is **not ready to tag today**, for two kinds of reason:

1. **Release logistics (blockers).**
   - None of the Marina / Old-McDonald / CAD work is committed.
   - Local `main` is behind `origin/main`, and the README conflicts.
   - `v1.0.0` is already tagged and published, so the version must be bumped.
   - The new code has never been compiled on Linux/GCC.
   - The archives in `dist/` are stale.
2. **Two verified data-corruption / data-loss defects in the editor (blockers).**
   - In the CAD and Old-McDonald workspaces, the **File menu Save/Load** and the **replay timeline Stop/Step** bypass the explode bookkeeping. The assembly gets permanently displaced, and that state can be written over the shipped project files.
   - A **failed scene load wipes the open scene**.

Each defect is a small, local fix (estimates in §5).

The product vision ("completeness and usability of a mature editor like Unity") is **not achievable in 48 hours**, and this audit does not pretend otherwise. PhisiksLab today is an inspector/viewer/editor for scenes built in C++. It has no undo, no asset import, no project creation, no entity/component creation, and no unsaved-changes protection. The realistic goal for this release is: ship what exists, fix the blockers, and describe the scope honestly. The editor-completeness features are listed in §4 as the next milestone.

---

## 2. Current build status

| Item | Result | How |
|---|---|---|
| Configure, `vs2026-x64` preset (PhysX + WASAPI) | **VERIFIED**: OK | [run] |
| Incremental build, `release` preset | **VERIFIED**: exit 0, 0 warnings | [run] |
| **Clean from-scratch build** (fresh build dir, Release, all targets) | **VERIFIED**: exit 0, **0 errors, 0 warnings**, 525 s | [run] |
| `cmake --install` (release layout) | **VERIFIED**: exit 0, 32 MB. Includes `projects/Old-McDonald/*`, `assets/`, `shaders/`, app-local MSVC runtime, PhysX DLLs | [run] |
| Installed `PhisiksLab.exe` imports | **VERIFIED**: every non-OS DLL import ships in the folder (dumpbin) | [run] |
| `assetgen` determinism | **VERIFIED**: 173 files regenerated, **0 differ** from committed `assets/`. The HeroMeshes→MeshKit refactor changed no output | [run] |
| Shipped project files vs current code | **VERIFIED**: `old_mcdonald`, `rover_cad`, `drone_cad` `.scene.json` are byte-identical to a fresh autotest save | [run] |
| Portable configuration (native physics + miniaudio) | Not built in this audit | — |
| Linux / GCC build of the **current** tree | **Not built.** CI last passed on README-only commits; none of the new files has gone through GCC | — |
| macOS | Never built (documented) | — |
| `dist/*.zip` | **BROKEN (stale)**: built 2026-11-22, before Marina/Old-McDonald. No `projects/`, no Marina/Old-McDonald meshes. `-win64.zip` and `-win64-physx.zip` are byte-identical (same MD5) | [run] |

---

## 3. Test results

| Suite | Result | Time | Notes |
|---|---|---|---|
| `PhisiksUnitTests` | **147 cases, 0 failed** | 2.5 s | headless |
| `ctest` (7 tests: unit, layering, deterministic replay, 4 showcase autotests) | **7/7 passed** | 103 s | |
| `PhisiksLab --autotest` (dev build, GL 4.6, PhysX) | **228 passed, 0 failed** | 150 s | covers Marina, Old-McDonald + mission, Rover CAD, Drone CAD |
| `PhisiksLab --autotest` from the **installed package**, launched from an unrelated working directory | **228 passed, 0 failed** | 140 s | asset/shader resolution next to the exe works |
| `PhisiksLab --autotest --gl41` (low-end tier) | **212 passed, 1 FAILED**: Old-McDonald mission step "timed out after 40.2 s sim / 2001 frames (phase 13)" (see H8) | 132 s | all other workspace checks pass on 4.1 |
| Showcase frame budget (from ctest run) | 80 FPS avg, p95 14.6 ms @ 1920×1080 Medium; peak RAM 384 MB | | |

**Coverage gap.** `PhisiksLab --autotest` is **not part of `ctest`**, so `ctest` alone never exercises the four engineering workspaces. The autotest also drives the workspaces through C++ calls, not through the File menu, inspector widgets, gizmo or replay transport. Every defect in §4.1 sits in exactly those untested UI paths.

### Manual / exploratory runs

| Run | Result |
|---|---|
| Each workspace boots standalone (`--scene ProjectMarina / OldMcDonald / RoverCAD / DroneCAD`) | **VERIFIED**: exit 0, 0 GL errors |
| `--scene Nope` | exits 1 with `Unknown initial scene 'Nope'` in the log; the valid scene names are not listed |
| Installed package in a folder **without write permission** | starts and renders. `could not open log file Build/runs/latest/engine.log` on stderr only; logs, crash dumps and saves are lost |

### Performance and memory (900 frames, visible window, default settings, Intel UHD)

| Workspace | Avg frame | ≈ FPS | Peak working set |
|---|---|---|---|
| Old-McDonald (613 entities) | 27.1 ms | **37** | 416 MB |
| Project Marina (347 entities) | 22.8 ms | **44** | 387 MB |
| Rover CAD | 15.1 ms | 66 | 364 MB |
| Drone CAD | 15.9 ms | 63 | 370 MB |
| Old-McDonald `--gl41 --quality low` | 9.4 ms | 106 | 360 MB |

Memory is well inside the 2.5 GB budget. The two project scenes miss 60 FPS on an iGPU at default quality, and the low preset fixes it, but nothing selects it automatically (§4.3). No long-running soak or leak test was done; the autotest's scene-unload checks pass.

---

## 4. Findings by area

### 4.1 Release blockers

| # | Area | Status | Finding | How |
|---|---|---|---|---|
| **B1** | Repo state | **BROKEN** | All engineering-workspace work is uncommitted. That is 13 modified and ~20 untracked source files, plus `projects/`, `assets/meshes/{marina,oldmcdonald}` and `docs/reference/`. Local `main` (`784cede`) is **3 commits behind `origin/main`** (`00ca194`, `e0ce5af`, `5b0452d`, all web edits to `README.md`), and the local README is also modified, so a conflict is likely. `Studio.cpp` shows a 1,227-line diff that is really a 13-line change plus LF→CRLF conversion; there is no `.gitattributes`. | [run] git + GitHub API |
| **B2** | Versioning | **BROKEN** | Tag `v1.0.0` and the GitHub release (with win64, win64-physx and linux archives) **already exist**. `project(Phisiks VERSION 1.0.0)` is unchanged, and `release.yml` requires tag == CMake version. A new public release needs a version bump (e.g. 1.1.0), and README/QUICKSTART must say `1.1.0`. | [run] |
| **B3** | Cross-platform | **INCOMPLETE** | About 3,500 lines of new C++ (Marina, Old-McDonald, CAD, ProjectKit, MeshKit) have only been compiled with MSVC. The release workflow publishes a Linux archive. Run CI on a branch before tagging, or drop Linux from this release. | [run] |
| **B4** | CAD / Old-McDonald persistence | **BROKEN** | **The explode state gets out of sync with the part positions, and the corruption can be saved over the shipped project files.** The workspaces explode by adding offsets to part positions and remember how far they have exploded. Only their own panel buttons ("Save project" / "Reopen") collapse and reset that state. Three other paths skip it:<br>• **File > Save JSON while exploded** writes the exploded layout. Its default path is the workspace's shipped project file (e.g. `projects/Old-McDonald/rover_cad.scene.json`). After a Reopen, the LiDAR sits at 0.468 instead of 0.168, "Exploded" moves it to 0.768, and "Assembled" can't return it.<br>• **File > Load JSON while exploded**: pressing "Assembled" afterwards drives the LiDAR to **−0.132**, through the chassis.<br>• **Replay timeline Stop / Step back** restores every entity's transform but not the explode state. The same −0.132 result.<br>The same mechanism exists in `OldMcDonaldScene` (two assemblies, plus mission state). | **[harness]** on `RoverCadScene`, all three reproduced; Old-McDonald [code] |
| **B5** | Scene persistence | **BROKEN** | **A failed load destroys the open scene.** `SceneSerializer::FromJson` clears the scene and only then reads the entities. A file that is valid JSON but has one malformed field leaves 2 of 148 entities. The UI reports "load failed" and the unsaved work is gone. The `version` field is never checked (a `version: 99` file loads). `Save` writes in place, with no temp file and rename. | **[harness]** |
| **B6** | Packaging | **BROKEN** | `dist/` archives are stale (§2). Rebuild them from the committed, version-bumped tree. Archive naming is also misleading: the CI `win64.zip` is native physics, the local `win64.zip` is PhysX, and the files share names. | [run] |

### 4.2 High-severity defects (fix before release if at all possible)

| # | Area | Status | Finding | How |
|---|---|---|---|---|
| H1 | Data safety | **MISSING** | No unsaved-changes tracking or prompt. Closing the window, pressing **Esc then Q**, switching scene from the menu, "Rebuild" and "Load JSON" all discard edits silently. With no undo (H2), one mis-click loses work. | [code] |
| H2 | Editing | **MISSING** | No undo/redo anywhere. The inspector's **DESTROY** button deletes the entity and its whole subtree at once, with no confirmation. | [code] |
| H3 | Packaged paths | **BROKEN** | Project Save/Reopen paths, the default `Build/scenes/scene.json`, `screenshots/` and the log/crash-dump directory `Build/runs/latest` are all **relative to the working directory**. QUICKSTART says the programs "work from any working directory". That holds for assets, but not for project files. From a shortcut or terminal elsewhere, Reopen fails and Save writes somewhere unexpected. In a read-only install folder, logs and crash dumps are lost (verified) and saves fail. | [run] read-only; [code] cwd |
| H4 | Inspector | **BROKEN** | The rotation field can't pass ±90° on Y. Each frame it re-derives Euler angles from the quaternion. Drag Y to 95 and the field jumps to (180, 85, 180), then the next drag step snaps it back to 80. Rotating a part past 90° about Y is only possible with the gizmo. | **[harness]** |
| H5 | Viewport | **BROKEN** | In F3 quad view the main camera is shrunk to the top-left quarter, but the gizmo is still drawn and hit-tested against the **full window** with the full-window aspect, so it is misaligned. The quad cameras are also hard-coded 40 m away with lesson-scale ortho heights, so in the CAD workspaces (assemblies under 1 m) the three ortho panes are nearly empty. The CAD workspaces' own six principal views work (VERIFIED). | [code] |
| H6 | Messaging | **INCOMPLETE** | Docs disagree with each other and with the product:<br>• README says "runs on Windows, Linux and macOS", but macOS has never been built and there is no macOS download.<br>• `KNOWN_ISSUES.md` still says "Windows/MSVC x64 only for now" and "Linux / macOS not yet built", which P12 contradicts.<br>• README says the black hole is "physically accurate", but KNOWN_ISSUES says Schwarzschild only, with a pseudo-Newtonian disk.<br>• README's joke-heavy tone doesn't fit an engineering product.<br>• The audit brief calls Old-McDonald a "greenhouse **simulation**". The project is honestly labelled as mission *playback*: A* routing and arm IK are real, but drone flight, mapping and measurements are scripted. Release notes must keep that wording. | [code] |
| H8 | Tests / low-end tier | **BROKEN** (test) · mission on 4.1 **unverified** | The autotest's mission step fails on the OpenGL 4.1 tier. Each step has a hard **2,000-frame cap** as well as its 600 s sim budget (`AutoTest.cpp:168`). On 4.6 the mission needed about 42 s of sim time and finished. On 4.1, frames are faster (about 20 ms), so the cap stopped it at 40.2 s, just short. This is almost certainly the harness budget, not a mission stall: A* and IK run on the CPU and don't depend on the GL tier. But the mission has **never completed on the 4.1 tier** in any recorded run; the older `Build/runs/*gl41*` reports predate Old-McDonald. Fix: let the mission step's frame cap scale with its sim budget, then re-run `--gl41`. | [run] |
| H7 | Toolchain | **INCOMPLETE** (risk) | The local PhysX build, and the bundled `msvcp140*/vcruntime140*` DLLs, come from **Visual Studio 18 Insiders** (pre-release) and **CMake 4.5.0-rc1**. Check whether the preview runtime may be redistributed, or build the public Windows archive with a stable toolset (CI uses VS 2022). I did not verify the licensing terms. | [run] |

### 4.3 Area-by-area status

**Application startup and project management**
- VERIFIED [run]: boots into Sandbox; `--scene` reaches every workspace; 0 GL errors; shader cache warm start 113 ms (46 programs); clean shutdown.
- VERIFIED [run]: an unknown scene fails cleanly (exit 1). INCOMPLETE: it prints no list of valid scenes, and on double-click the error only flashes in the console window.
- MISSING: project management. There is no New / Open / Recent project. "Projects" are four scenes hard-coded in C++ under *Simulation > Projects*, and `project.json` is informational only (it says so itself). There is no file dialog; paths are typed into a text field.
- INCOMPLETE: first launch drops the user into a first-person physics sandbox with a crosshair. For an engineering visualisation product, the first screen should be a workspace/project chooser. Changing that is a product decision; it is low-cost.
- INCOMPLETE: Marina's project path is `projects/project_marina.json`, while the others live in `projects/<Name>/`. No Marina project file ships, so Reopen before the first Save reports "load failed".

**Editor layout and workspace navigation**
- VERIFIED [run]: menu bar, left workspace panel (classroom or workspace-specific), inspector, hierarchy, telemetry and timeline all render (16,746 UI vertices); Tab hides the panels; presentation mode draws the title block (autotest screenshots).
- INCOMPLETE [code]: panels sit at fixed pixel positions with no docking, resizing or saved layout. There is **no DPI scaling** (no content-scale handling in `engine/src/ui`), so on 150–200 % Windows scaling the UI will be small. Not tested on a high-DPI display or at 1366×768.

**Scene hierarchy and inspector**
- VERIFIED [run, autotest]: hierarchy-to-inspector selection; descendant counts; rename; transform, material, light, collider, rigidbody and joint editing.
- BROKEN: rotation past ±90° (H4). MISSING: undo (H2).
- MISSING: search/filter (Old-McDonald has 613 entities), drag-and-drop re-parenting, multi-select, create entity, add/remove component, duplicate. The inspector shows the mesh name as read-only text.
- INCOMPLETE: material edits change the **shared** material, i.e. every part using it, with no "make unique". Probably the intended behaviour, but it isn't signalled.

**3D viewport and camera controls**
- VERIFIED [run]: orbit/pan/zoom; CAD perspective + six orthographic views with mouse-wheel zoom in ortho; Old-McDonald's 8 camera presets.
- VERIFIED [harness]: gizmo maths. ImGuizmo's decomposition and the Euler convention in `Studio.cpp` agree, and there is no rotation or scale drift over a 600-frame drag.
- BROKEN: gizmo in quad view (H5). MISSING: frame/focus on selection (an F key), gizmo snapping, W/E/R mode shortcuts (W is a movement key).

**Object selection, manipulation and transforms**
- VERIFIED [run]: picking via the entity-ID buffer in every workspace and every CAD view (13–211 distinct parts per view); subsystem select; select parent assembly; hide selected / show all; translate/rotate/scale gizmo, world/local.
- Quad-view picking was not verified at runtime.

**Asset importing and management**
- VERIFIED [run]: on-demand OBJ / glTF / WAV / PNG loading; background texture streaming; deterministic asset generation.
- **MISSING**: any user-facing import. There is no asset browser, no import command, and no way to attach a user's OBJ/glTF to an entity from the UI. Adding user geometry requires C++. This is the largest gap against the "engineering visualisation application" vision.

**Scene persistence and project save/load**
- VERIFIED [run]: round-trip through the workspaces' own buttons. Marina: 348 entities, opacity kept. Old-McDonald: 629 entities, edits kept, navigation re-attached. Rover CAD and Drone CAD: edits kept.
- BROKEN: B4 (File menu and timeline vs explode), B5 (failed load wipes the scene; no version check; non-atomic write), H3 (cwd-relative paths).
- INCOMPLETE (documented): JSON does not store Joint, Script, NavAgent, Terrain, Decal, Trail, SkinnedModel or CameraPath. **File > Save on the lesson / P9 scenes silently drops those components**, with no warning at save time.

**Simulation and play/stop workflows**
- VERIFIED [run]: pause stops simulation (sim time unchanged over 15 frames); deterministic replay byte-identical (ctest); replay ring 30 s.
- INCOMPLETE: there is no edit mode vs play mode. Physics runs from launch. "Stop" on the timeline rewinds transforms and bodies to the first recorded step, but it doesn't revert other edits made during play. It also breaks the CAD and project workspaces (B4). The project scenes have their own mission Play/Pause/Reset, separate from the global transport, so the UI shows two transports.

**Marina workspace**
- VERIFIED [run]: boots; 211 individually pickable parts; save/reopen unchanged (348 entities); tubing runs continuous (worst gap 5e-8 m); see-through materials survive reopen; presentation mode.
- Honest labelling is in place (flow animation, no simulation). INCOMPLETE: no shipped project file (see startup); 44 FPS on an iGPU.

**Old-McDonald greenhouse (mission playback)**
- VERIFIED [run]: nav grid built from colliders (1,524 walkable cells, 21 ms); drone maps 48/48 crop locations and lands before the rover leaves; rover samples 16/16 points by A* with 0 path failures and docks within 0.027 m; exploded drone and rover; 9 subsystem toggles; edits survive save + reopen.
- BROKEN: B4 class, from code: two explode states plus mission state, and File Load / timeline Stop bypass them.
- UNVERIFIED on the OpenGL 4.1 tier: the autotest's mission step hits its frame cap before docking (H8).
- INCOMPLETE: 37 FPS at default quality on an iGPU. Drone flight, mapping and measurements are scripted (documented, correctly labelled).

**Standalone Rover CAD workspace**
- VERIFIED [run]: assembly isolated; same hierarchy as the mission (17/17 groups); 111 render objects; wiring continuous; 7 views pickable; explode and collapse; hidden subsystem and component leave the image; save + reopen; arm-joint sliders; stow.
- BROKEN: B4 (all three paths reproduced [harness]).

**Drone CAD workspace**
- VERIFIED [run]: same checks as Rover CAD (23/23 groups, 158 render objects, 23 wiring runs, 7 views, explode with assembly lift clearing the ground, collapse to skids on the ground, save + reopen).
- BROKEN: B4 (same `CadWorkspace` code path [code]).

**Performance and memory**
- VERIFIED [run]: peak 360–416 MB; showcases at 80 FPS @ 1080p Medium.
- INCOMPLETE: project scenes reach 37–44 FPS on an iGPU at default settings; there is no automatic quality selection for weak GPUs, and the low preset gives 106 FPS. No soak test.

**Packaging and clean-machine execution**
- VERIFIED [run]: install layout; DLL closure; installed package passes the full autotest from a foreign working directory; starts from a read-only folder.
- BROKEN: stale `dist/` (B6); logs and saves lost in read-only or foreign-cwd installs (H3).
- **Not verified: a true clean machine.** This machine has VS installed, so a missing runtime would not show up. Windows Sandbox isn't available on Windows 11 Home, so test on a second PC or a fresh VM. Linux archive never launched on a desktop (documented). Unsigned binaries trigger SmartScreen, and a console window opens on double-click (documented).

---

## 5. Recommended fixes, ranked by severity

| Rank | Fix | Addresses | Est. |
|---|---|---|---|
| 1 | Create a release branch. Commit the workspace work in logical commits, add `.gitattributes` (`* text=auto eol=lf` for sources), merge the 3 remote README commits and resolve the README conflict. Decide whether `docs/reference/*.jpeg` (personal prototype photos) belong in a **public** repo. | B1 | 1–2 h |
| 2 | **Explode-state integrity.** (a) Add `OnBeforeSave()/OnAfterSave()/OnAfterLoad()` hooks to `StudioExtension` and call them from the File menu's Save/Load in `Studio.cpp`, as `ProjectFileButtons` already does. (b) In `StudioExtension` scenes, disable replay recording/timeline, or call `OnAfterLoad` after `Replay::Restore`. (c) Add autotest checks for all three paths. | B4 | 2–3 h |
| 3 | **Load safety.** Parse and validate into an intermediate list before `scene.Clear()`; reject unknown `version`; write saves to `path.tmp` then rename. Add a unit test with a malformed file. | B5 | 2 h |
| 4 | Bump to **1.1.0** (CMake, QUICKSTART, README); give the PhysX archive a distinct name (`-win64-physx.zip`) in CPack rather than renaming by hand. | B2, B6 | 0.5 h |
| 5 | Push the branch and get **CI green on Linux (GCC) and Windows (portable)**. Fix any GCC errors in the new files. Fallback: release Windows only and say so. | B3 | 2–6 h (unknown until run) |
| 6 | Resolve project files the way assets are resolved (next to the exe first). Send logs, crash dumps and default saves to a writable per-user folder (`%LOCALAPPDATA%\Phisiks`, as the shader cache already does) when the cwd isn't writable. | H3 | 2 h |
| 7 | Unsaved-changes guard: a dirty flag set by inspector, gizmo and workspace edits; a confirm dialog on close, Q-quit, scene switch, Rebuild and Load. A confirm on DESTROY. | H1, part of H2 | 3 h |
| 8 | Inspector rotation: cache the displayed Euler angles per selected entity and only rebuild the quaternion from user edits (don't re-derive every frame). | H4 | 1 h |
| 9 | Docs truth pass: README platforms = Windows + Linux; remove "physically accurate" and the jokes; refresh KNOWN_ISSUES (remove stale P8 platform notes; add B5/H5 if not fixed, plus the components JSON drops); release notes use "mission playback", never "simulation", for Old-McDonald. | H6 | 1–2 h |
| 10 | Gizmo in quad view: use the main camera's viewport rect and aspect in `Gizmo()`, or hide the gizmo while quad view is on. | H5 | 1 h |
| 11 | Raise or remove the 2,000-frame cap for long steps (H8) and re-run `--autotest --gl41` until it passes. Add `PhisiksLab --autotest --hidden` to `ctest` so the workspaces are covered by the standard test command. | H8, coverage | 1 h |
| 12 | Build the public Windows/PhysX archive with a stable toolset (VS 2022/2026 GA, CMake 3.x/4.x release). | H7 | 1 h + build |
| 13 | Ship a pristine Marina project file at `projects/Project-Marina/project_marina.scene.json` for consistency. | startup | 0.5 h |
| — | **Defer to the next milestone** (not achievable safely in 48 h): undo/redo; asset import + asset browser; New/Open/Recent project + file dialogs; create entity / add component / re-parent / search; docking layout + DPI scaling; edit-mode vs play-mode separation; automatic quality preset for weak GPUs; serialising the remaining component types. | §4.3 | weeks |

---

## 6. Realistic 48-hour release plan

Assumes one developer. "Gate" means: do not continue until it passes.

**Day 1: make it correct**

| Hours | Work | Gate |
|---|---|---|
| 0–2 | Fix 1: release branch, commits, `.gitattributes`, merge remote README, decide on `docs/reference/` | clean `git status`, branch pushed |
| 2–3 | Push branch to trigger CI (Linux GCC + Windows portable) **early**, so failures surface while other work proceeds | CI running |
| 3–6 | Fix 2 (explode integrity) + autotest checks | Lab autotest all pass, new checks included |
| 6–8 | Fix 3 (load safety) + unit test | unit tests pass |
| 8–10 | Address CI results (Fix 5) | CI green, or decision: Windows-only release |
| 10–12 | Fix 4 (version 1.1.0, archive names) + Fix 11 (autotest in ctest) | ctest passes |
| 12–15 | Fix 6 (paths/logs) + Fix 8 (rotation field) | manual check from a foreign cwd + read-only folder |
| 15–18 | Fix 7 (unsaved-changes guard) — **cut first if behind schedule**; if cut, document it in KNOWN_ISSUES | manual check |

**Day 2: prove it and ship**

| Hours | Work | Gate |
|---|---|---|
| 18–20 | Fix 9 (docs truth pass), Fix 10 (gizmo/quad) if time allows | docs reviewed against §4 |
| 20–24 | Full matrix: PhysX + native × GL 4.6 + GL 4.1, Lab autotest + ctest; workspace perf spot-check | 0 failures |
| 24–28 | Build archives (Fix 12: stable toolset); install; **clean-machine test on a second PC or fresh VM**: launch each workspace from Explorer, save + reopen each project, screenshot, quit | all four workspaces work on the clean machine |
| 28–32 | Exploratory pass on the release build: every menu item, every workspace panel button, File Save/Load in each workspace, timeline transport in each workspace, DESTROY, Tab | no new blockers |
| 32–36 | Tag `v1.1.0` and let `release.yml` build; download the CI archives and smoke-test them | archives launch |
| 36–40 | Publish the release with honest notes: scope = engineering visualisation viewer/editor for the bundled workspaces; known limitations (no undo, no asset import, no project creation); Windows (+ Linux if CI-verified) | release live |
| 40–48 | **Buffer.** Reserved for CI or clean-machine surprises. Do not spend it on new features. | — |

**Ship-blocking criteria:** B1–B6 fixed, CI green for every platform that gets an archive, a clean-machine run passed, and the docs match §4.

**Explicitly out of scope for this release:** everything in the "defer" row of §5. Announcing "Unity-level editor completeness" with this release would be inaccurate. The current product is a polished engineering-visualisation viewer and editor for four bundled workspaces, backed by a well-tested engine.
