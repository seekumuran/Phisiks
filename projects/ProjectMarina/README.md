# Project Marina: Waste-Heat Membrane Distillation

An example project built on the PHISIKS engine and editor: an editable engineering scene of a waste-heat membrane-distillation desalination prototype. The heat a single-board computer gives off drives a membrane distillation cell that produces fresh water from seawater.

This is a separate application (`ProjectMarina`), not part of the PHISIKS editor itself.

## Run it

| | |
|---|---|
| Release package | `ProjectMarina` (`ProjectMarina.exe` on Windows), next to `Phisiks` |
| Development build | `Build/bin/Release/ProjectMarina` (run from the repository root) |
| Scene file | `projects/ProjectMarina/scenes/project_marina.scene.json` |
| Meshes | `projects/assets/meshes/marina` |

**File > Save** (or the panel's **Save**) writes the scene file, and **Reopen** loads it. **Rebuild** rebuilds the scene from code. Scene files are found next to the executable first, then in the working directory. If that folder is read-only, edits go to a per-user copy.

## What's in the scene

| Subsystem | Contents |
|---|---|
| Bench | aluminium tooling plate, electronics sub-plate, splash wall, slotted wiring duct |
| 1 SBC | Raspberry Pi format computer with a SoC water block and a 30 mm fan (the heat source) |
| 2 Cooling | closed coolant loop SBC -> pump P1 -> heat exchanger HX1 -> SBC (clear tubing) |
| 3 Electronics | UNO-format controller, ESP32-C3 board, breadboard, MOSFET pump driver |
| 4 Pump P1 | DC diaphragm pump on a strapped bracket |
| 5 HX1 | brazed-plate heat exchanger (coolant <-> seawater feed) |
| 6 MD1 | enclosed flat-sheet membrane distillation cell, hot side / cold side |
| 7 TK1 | seawater reservoir (feed out, brine return) |
| 8 Condensation | finned condenser CD1 on a stand above the freshwater bottle TK2 |
| 9 Instruments | temperature probes T1-T4, sensor cables in the duct, jumper wiring |

Every part is its own entity, so you can select, move and re-material it, grouped per subsystem in the hierarchy. Tubes and wires are chains of straight and 90 degree bend pieces, each one a scene entity. Materials are saved with the scene, so save and reopen round-trips the whole project.

Parts without specified hardware geometry are generic models of their component class, sized from public board outlines where those exist. No manufacturer CAD data or artwork is used. The feed-side circulation drive is not specified in the brief, so the only pump is the one in the coolant loop.

## What it does and doesn't do

- The optional **flow direction** animation moves dashes along each fluid run in its intended direction. It is schematic: constant speed, no data.
- **No thermal or fluid simulation is run, and no measured values are shown.**
- The fan rotor spins for visual effect only.

## Workspace panel

- **Camera:** Perspective, Front, Top plan, Electronics, Thermal loop, Desalination.
- **Display:** CAD edges; fan spin; flow direction, with its speed and the circuit colours.
- **Subsystems:** visibility box and selection for each subsystem; select the parent assembly.
- **Scene file:** Save, Reopen, Rebuild, and **Screenshot (PNG)** to `screenshots/`.

## Files

| Path | What it is |
|---|---|
| `scenes/project_marina.scene.json` | the scene (JSON, `phisiks-scene` v1) |
| `src/MarinaSpec.h` | placements, ports and dimensions (shared by the scene and the mesh generator) |
| `src/MarinaScene.cpp` | builds the scene; flow animation; panel |
| `src/Main.cpp` | the `ProjectMarina` executable |
| `../common/ProjectKit.cpp` | building blocks shared with Old-McDonald: parts, tube / wire runs, visibility, scene-file buttons |
| `../tools/MarinaMeshes.cpp` | generates `projects/assets/meshes/marina/*.obj` and the tubing / wiring kit (CC0, deterministic; `projectassetgen`) |

## Verification

`PhisiksRuntimeTests --autotest` (ctest `RuntimeTests`, `RuntimeTestsGL41`) checks:

- per-part picking (entity-id buffer);
- save and reopen unchanged;
- parts and runs rebuilt after reopening, with a worst run gap under 0.1 mm;
- see-through tubing keeping its opacity;
- no text or overlay drawn over the scene with the panels hidden.
