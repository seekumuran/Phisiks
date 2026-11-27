# Old-McDonald: Autonomous Agricultural Robotics

An example project built on the PHISIKS engine and editor: an editable engineering scene of an autonomous greenhouse monitoring system. A mapping drone surveys the crops, then a ground rover visits a fixed soil-sampling grid, and its arm places soil sensors at each point. Everything is built from ordinary scene entities, so you select, move, edit, save and reopen it with the standard editor tools.

This is a separate application (`OldMcDonald`), not part of the PHISIKS editor itself.

## Run it

| | |
|---|---|
| Release package | `OldMcDonald` (`OldMcDonald.exe` on Windows), next to `Phisiks` |
| Development build | `Build/bin/Release/OldMcDonald` (run from the repository root) |
| Workspaces | **Workspace** menu: *Greenhouse mission* (default), *Rover CAD*, *Drone CAD*; or `--scene OldMcDonald / RoverCAD / DroneCAD` |
| Scene files | `projects/OldMcDonald/scenes/old_mcdonald.scene.json`, `rover_cad.scene.json`, `drone_cad.scene.json` |
| Meshes | `projects/assets/meshes/oldmcdonald` (plus the tubing kit in `projects/assets/meshes/marina`) |

**File > Save** (or the panel's **Save**) writes the workspace's scene file. Both assemblies are always saved collapsed, whatever the explode state. **Reopen** loads it, and **Rebuild** rebuilds the scene from code. Scene files are found next to the executable first, then in the working directory. If that folder is read-only, edits are saved to a per-user copy (`%LOCALAPPDATA%\Phisiks\…`, `~/Library/Application Support/Phisiks/…`, `~/.local/share/phisiks/…`).

## What's in the scene

| Group | Contents |
|---|---|
| `1 Greenhouse` | 6 x 4 m slab, aluminium frame, wall and roof glazing, rover dock bay, drone landing pad, wall colliders |
| `2 Growing Beds` | three raised timber beds (with colliders) and 48 crops, each with a crop-location ring |
| `3 Mapping Drone` | F450-class quad, grouped by subsystem: **Frame** (slotted black top plate, power distribution bottom plate, four black lattice arms in X, arm screws, X motor mounts, cable ties), **Propulsion** (4 brushless motors with red bell bands), **Propellers** (10 inch, bullet prop nuts), **Electronics** (**Pixhawk 2.4.8** flight controller on a damping mount, 4 ESCs under the arms), **GPS - ReadyToSky** on a mast (rear), **Battery** (LiPo, under the frame), **Landing gear**, **Drone wiring** (ESC leads, GPS cable, battery leads) |
| `4 Soil Rover` | black box chassis, black top plate with a chamfered nose on hex standoffs, yellow fenders (L / R) on brackets and a yellow sloped front panel, a raised arm mounting platform, four gearmotor-driven wheels (block-tread tyre, blue Y-spoke rim, hub), **Raspberry Pi Pico** (RP2040, not Pico W) on a carrier, motor driver, battery pack, 2D **LiDAR** on a mast, the servo-and-bracket **sensor-positioning arm** (J1 yaw, J2 shoulder, J3 elbow, J4 wrist; blue servos, black U brackets, aluminium-tube forearm, a non-articulated wrist-rotate servo and gripper), **soil sensor head** held in the gripper with the moisture, temperature and pH probes, deck wiring |
| `5 Sampling Grid` | 16 fixed points `SP-A1` .. `SP-D4` (rows A-D x columns 1-4), each a selectable disc and stake, plus row lines |
| `6 Mission Route` | the rover route (planned by the engine's A*, see below) and the drone survey path |

The arm has one job: putting the three soil probes at a sampling point. It doesn't harvest, spray or manipulate anything else. Only the three confirmed soil sensors are modelled. There are no nutrient sensors.

Where a part has public outline dimensions, the model uses them: Pixhawk 2.4.8 case 81.5 x 50 x 15.5 mm, Pico 51 x 21 mm, 450 mm frame diagonal, 10 inch props. Everything else is a generic model of its component class. The scenes carry no text labels; the hierarchy names every part.

### Rover model vs. the prototype

The rover's look comes from one photograph of the prototype; the photograph is not part of this repository. No dimensions were measured from the real prototype.

- **Taken from the photo:**
  - colours and materials;
  - the panel layout: black chassis and top plate, yellow fenders and front panel;
  - the tyre and rim style;
  - the arm's construction: servos in sheet brackets, tube forearm, gripper.
- **Kept from the original layout:**
  - wheel size (140 mm), track and wheelbase;
  - the arm link lengths (300 / 260 mm). The mission's sampling reach depends on them. In the photo the real arm looks shorter relative to the chassis.
- **Interpretation:**
  - The photo shows a gripper but not how the sensors attach, so the probe plate is shown held in the gripper jaws.
  - The wrist-rotate and gripper servos are modelled but not driven. The IK uses J1-J4 only.
- **Not visible in the photo:** the Pico, the driver, the battery and the LiDAR. They stay at their documented positions.

### Drone model vs. the prototype

The drone's look comes from one photograph taken from above while the drone is held in a hand; the photograph is not part of this repository. No dimensions were measured from the real prototype.

- **Taken from the photo:**
  - all-black lattice arms in X;
  - a black slotted top plate;
  - red bands on the motor bells, and silver prop nuts;
  - dark two-blade props;
  - a black Pixhawk case;
  - a black GPS puck on a mast;
  - white cable ties on the arms;
  - a blue-wrapped battery under the frame.
- **Kept from the documented layout:** the F450-class 450 mm motor diagonal, 10 inch props, the Pixhawk 2.4.8 case size, and the ReadyToSky GPS on a mast.
- **Interpretation:** the yellow blocks under the arms are drawn as the ESCs (yellow heat-shrink). The photo doesn't show them clearly enough to confirm.
- **Not visible in the photo:** the landing gear and the bottom plate. They stay as generic models.

## Mission playback: what is computed, and what is scripted

The **Mission** camera preset, or the panel's Play button, runs the operational sequence in order. The drone and the rover never run at the same time, and they don't coordinate live.

1. The drone surveys the greenhouse.
2. The drone maps crop locations.
3. Mapping data is transferred and processed.
4. The fixed soil-sampling grid is generated.
5. The rover begins its mission.
6. The rover visits the sampling locations.
7. The arm positions the soil sensors.
8. Measurements are collected.
9. The rover returns to its dock.

| Computed by the engine | Scripted (visual only) |
|---|---|
| The rover route: PHISIKS **A\*** on a navigation grid built from the scene's static colliders (floor, beds, walls), followed by `NavAgent` steering. Move a bed and the route changes. | The drone's flight: a constant-speed spline. There is no flight dynamics and no flight control. |
| The arm pose at every sampling point: analytic **inverse kinematics** (yaw + planar two-link + wrist kept vertical). | Crop "mapping": crop rings are marked when they fall under an assumed 1.2 x 0.9 m footprint. This is not a camera or image-processing model. |
| | Data transfer and processing, and the measurements: timed steps. No sensor values are produced or shown. |

The panel lists the nine stages, each tagged with what drives it: `[A*]`, `[IK]` or `[scripted]`.

## Workspace panel (left column, greenhouse mission)

- **Camera:** Perspective, Drone close, Rover close, Overview, Top plan (orthographic), Drone explode, Rover explode, Mission.
- **Mission playback:** play, pause, reset, follow camera, speed, the nine stages (active one highlighted), and counters for crops mapped, samples taken and path failures.
- **Display:** CAD edges, spin (propellers, LiDAR head, wheels), and explode sliders for the drone and the rover.
- **Subsystems:** visibility boxes and selection for the greenhouse, beds, drone, rover, robotic arm, LiDAR, soil sensors, sampling grid and mission route.
- **Scene file:** Save, Reopen, Rebuild, and **Screenshot (PNG)**, which writes the 3D view to `screenshots/`.

**Video:** export a PNG frame sequence with `OldMcDonald --export-dir <dir> --export-frames`. PHISIKS has no built-in video encoder, so assemble the frames with any external tool.

## Standalone CAD workspaces (Rover CAD, Drone CAD)

The rover and the drone can each be inspected on their own, away from the greenhouse (**Workspace** menu).

- **Same assembly, not a copy.** The greenhouse scene and the workspaces call one builder per vehicle (`RoverAssembly::Build`, `DroneAssembly::Build`). They share meshes, materials, hierarchy, explode offsets and wiring, so a change to a vehicle shows up everywhere. Each scene holds its own live instance; the greenhouse scene keeps its own vehicles for the mission.
- **Same workspace for both.** Both subclass `CadWorkspace` (`projects/common/CadWorkspace.cpp`). It contains only the assembly, a light-grey ground grid and three studio lights in `CAD Studio`.
- **Views:** Perspective, plus orthographic Front, Rear, Left, Right, Top and Bottom (assembly +X = front, +Z = right). The ground grid hides itself in the Bottom view, and the mouse wheel zooms in every view.
- **Panel:**
  - Assembled / Exploded buttons and an explode slider.
  - Rover only: sliders for arm joints J1-J4, and a Stow button.
  - CAD edges and Ground toggles.
  - The subsystem list (visibility box + select).
  - **Hide selected** / **Show all**.
  - The selected part's name, subsystem and material. Transform and material are edited in the inspector.
  - Save / Reopen / Rebuild.
- **Rover subsystems:** chassis, bodywork, drive, electronics, LiDAR, arm, end effector, sensor head, wiring.
- **Drone subsystems:** frame, motors, propellers, flight controller + ESCs, wiring, battery, GPS, landing gear.
- **Drone explode:**
  - the flight controller and GPS move up;
  - the battery and landing gear move down;
  - the arms, motors, props and ESCs move outward;
  - the whole drone lifts 0.2 m so nothing goes below the ground grid.
- The replay timeline is disabled in these workspaces: their state includes the explode offsets, which a transform replay would get out of step with.

## Files

| Path | What it is |
|---|---|
| `scenes/old_mcdonald.scene.json` | the project scene: entities, transforms, components and materials (JSON, `phisiks-scene` v1) |
| `scenes/rover_cad.scene.json`, `scenes/drone_cad.scene.json` | the CAD workspace scene files |
| `project.json` | project metadata (informational) |
| `src/OldMcDonaldSpec.h` | dimensions, placements, sampling grid, arm geometry (shared by the scene and the mesh generator) |
| `src/OldMcDonaldScene.cpp` | builds the project and runs the mission sequence and panel |
| `src/RoverAssembly.cpp`, `src/DroneAssembly.cpp` | the vehicle assemblies (parts, hierarchy, materials, explode, arm pose / wiring), shared by all scenes |
| `src/RoverCadScene.cpp`, `src/DroneCadScene.cpp` | the CAD workspaces (`CadWorkspace` + the rover's arm controls) |
| `src/Main.cpp` | the `OldMcDonald` executable (editor host + Workspace menu) |
| `../common/ProjectKit.cpp`, `../common/CadWorkspace.cpp` | building blocks shared with Project Marina; the CAD workspace framework |
| `../tools/OldMcDonaldMeshes.cpp` | generates `projects/assets/meshes/oldmcdonald/*.obj` (CC0, deterministic; `projectassetgen`) |

Saved scenes don't store the navigation components (`NavigationSurface`, `NavAgent`). The scene re-attaches them after every load, and the navigation grid is rebuilt from the saved colliders.

## Verification

`PhisiksRuntimeTests --autotest` (ctest `RuntimeTests`, `RuntimeTestsGL41`) runs these Old-McDonald steps.

- **Project:**
  - the hierarchy and named hardware;
  - the 16 sampling points;
  - a render object for every part, and continuous wiring runs;
  - the A* route;
  - per-component picking in the close-up views, and the top plan showing all 16 points;
  - the exploded views and subsystem visibility;
  - edits surviving save and reopen, with runtime navigation re-attached;
  - no text or overlay drawn over the scene with the panels hidden.
- **Mission:**
  - all nine stages run in order;
  - all 48 crops mapped, and the drone landed before the rover left;
  - all 16 points sampled with no A* failures;
  - the rover back within 0.1 m of its dock.
- **Rover CAD and Drone CAD:**
  - views, picking, explode / collapse and visibility;
  - edits surviving save and reopen;
  - File save / open while exploded keeping the assembly intact;
  - the replay timeline disabled.
