# Old-McDonald: Autonomous Agricultural Robotics

An editable engineering visualisation of an autonomous greenhouse monitoring system: a mapping drone surveys the crops, then a ground rover visits a fixed soil-sampling grid and its arm places soil sensors at each point. It is a regular PhisiksLab scene built from ordinary entities. You select, move, edit, save and reopen it with the standard studio tools. There is no special editor.

## Open it

- `PhisiksLab --scene OldMcDonald`, or **Simulation > Projects > Old-McDonald** in the Lab.
- **File > Load JSON**, or the panel's **Reopen** button, loads `projects/Old-McDonald/old_mcdonald.scene.json` (the studio's project path while the scene is open). **Save project** writes your edits back to that file. Exploded views are collapsed before saving.

## What's in the scene

| Group | Contents |
|---|---|
| `1 Greenhouse` | 6 x 4 m slab, aluminium frame, wall and roof glazing, rover dock bay, drone landing pad, wall colliders |
| `2 Growing Beds` | three raised timber beds (with colliders) and 48 crops, each with a crop-location ring |
| `3 Mapping Drone` | F450-class quad styled after the prototype photo (`docs/reference/old mcdonald/drone-reference.jpeg`), grouped by subsystem: **Frame** (slotted black top plate, power distribution bottom plate, four black lattice arms in X, arm screws, X motor mounts, cable ties), **Propulsion** (4 brushless motors with red bell bands), **Propellers** (10 inch, bullet prop nuts), **Electronics** (**Pixhawk 2.4.8** flight controller on a damping mount, 4 ESCs under the arms), **GPS - ReadyToSky** on a mast (rear), **Battery** (LiPo, under the frame), **Landing gear**, **Drone wiring** (ESC leads, GPS cable, battery leads) |
| `4 Soil Rover` | styled after the prototype photo (`docs/reference/old mcdonald/rover-reference.jpeg`): black box chassis, black top plate with a chamfered nose on hex standoffs, yellow fenders (L / R) on brackets and a yellow sloped front panel, a raised arm mounting platform, four gearmotor-driven wheels (block-tread tyre, blue Y-spoke rim, hub), **Raspberry Pi Pico** (RP2040, not Pico W) on a carrier, motor driver, battery pack, 2D **LiDAR** on a mast, the servo-and-bracket **sensor-positioning arm** (J1 yaw, J2 shoulder, J3 elbow, J4 wrist; blue servos, black U brackets, aluminium-tube forearm, a non-articulated wrist-rotate servo and gripper), **soil sensor head** held in the gripper with the moisture, temperature and pH probes, deck wiring |

### Rover model vs. the prototype photo

The rover's look comes from one photograph. No dimensions were measured from the real prototype.

- **Taken from the photo:** colours and materials, the panel layout (black chassis and top plate, yellow fenders and front panel), the tyre / rim style, and the arm's construction (servos in sheet brackets, tube forearm, gripper).
- **Kept from the original layout:** wheel size (140 mm), track and wheelbase, and the arm link lengths (300 / 260 mm). The mission's sampling reach depends on those link lengths. In the photo the real arm looks shorter relative to the chassis.
- **Interpretation:** the photo shows a gripper but not how the sensors attach, so the probe plate is shown held in the gripper jaws. The wrist-rotate and gripper servos are modelled but not driven. The IK uses J1-J4 only.
- **Not visible in the photo:** the Pico, the driver, the battery and the LiDAR. They stay at their documented positions.
| `5 Sampling Grid` | 16 fixed points `SP-A1` .. `SP-D4` (rows A-D x columns 1-4), each a selectable disc and stake, plus row lines |
| `6 Mission Route` | the rover route (planned by the engine's A*, see below) and the drone survey path |

The arm has one job: putting the three soil probes at a sampling point. It doesn't harvest, spray or manipulate anything else. Only the three confirmed soil sensors are modelled. There are no nutrient sensors.

Where a part has public outline dimensions, the model uses them (Pixhawk 2.4.8 case 81.5 x 50 x 15.5 mm, Pico 51 x 21 mm, 450 mm frame diagonal, 10 inch props). Everything else is a generic model of its component class, and the callouts say "generic" where that applies.

### Drone model vs. the prototype photo

The drone's look comes from one photograph, taken from above while the drone is held in a hand. No dimensions were measured from the real prototype.

- **Taken from the photo:**
  - all-black lattice arms in X (the old red and grey arms are gone);
  - a black slotted top plate;
  - red bands on the motor bells and silver prop nuts;
  - dark two-blade props;
  - a black Pixhawk case;
  - a black GPS puck on a mast;
  - white cable ties on the arms;
  - a blue-wrapped battery under the frame.
- **Kept from the documented layout:** the F450-class 450 mm motor diagonal, 10 inch props, the Pixhawk 2.4.8 case size, and the ReadyToSky GPS on a mast.
- **Interpretation:** the yellow blocks under the arms are drawn as the ESCs (yellow heat-shrink). The photo doesn't show them clearly enough to confirm.
- **Not visible in the photo:** the landing gear and the bottom plate. They stay as the generic models.

## Standalone rover CAD workspace

The rover can also be inspected on its own, away from the greenhouse:

- `PhisiksLab --scene RoverCAD`, or **Simulation > Projects > Old-McDonald Rover CAD** in the Lab.
- **Same assembly, not a copy.** Both scenes call one builder, `RoverAssembly::Build` (`apps/lab/src/RoverAssembly.cpp`). They share the same meshes, materials, hierarchy, explode offsets, arm stow pose and wiring, so a change to the rover shows up in both. Each scene holds its own live instance. The greenhouse scene keeps its rover for the mission.
- **Isolated.** The workspace contains only the rover, plus a light-grey ground grid and three studio lights in `CAD Studio`. There is no greenhouse, bed, dock, floor marking, drone or route.
- **Views:** Perspective, plus orthographic Front, Rear, Left, Right, Top and Bottom. The rover's +X is its front and +Z its right side. The ground grid hides itself in the Bottom view, and the mouse wheel zooms in every view.
- **Panel:**
  - Assembled / Exploded buttons and an explode slider.
  - Sliders for arm joints J1-J4, and a Stow button.
  - Callouts, CAD edges and Ground toggles.
  - A subsystem list (visibility box + select) for chassis, bodywork, drive, electronics, LiDAR, arm, end effector, sensor head and wiring.
  - **Hide selected** and **Show all**, for individual parts.
  - Save project, Reopen and Rebuild, on `projects/Old-McDonald/rover_cad.scene.json`.
- Every part is an ordinary entity. You select, move, rotate, scale and re-material it with the normal studio tools. Saves are written with the rover assembled.

## Standalone drone CAD workspace

- `PhisiksLab --scene DroneCAD`, or **Simulation > Projects > Old-McDonald Drone CAD** in the Lab.
- **Same assembly, not a copy.** Both scenes call `DroneAssembly::Build` (`apps/lab/src/DroneAssembly.cpp`), the same as the greenhouse scene. The greenhouse drone still flies the scripted survey.
- **Same workspace as the rover.** Both workspaces subclass `CadWorkspace` (`apps/lab/src/CadWorkspace.cpp`), so they share:
  - the views and the explode controls;
  - the display toggles and the subsystem list;
  - Hide selected / Show all;
  - the selected part's name, subsystem and material (transform and material are edited in the inspector);
  - Save / Reopen / Rebuild.
- **Subsystems:** frame, motors, propellers, flight controller + ESCs, wiring, battery, GPS and landing gear.
- **Project file:** `projects/Old-McDonald/drone_cad.scene.json`.
- **Explode:**
  - the flight controller and GPS move up;
  - the battery and landing gear move down;
  - the arms, motors, props and ESCs move outward;
  - the whole drone lifts 0.2 m so nothing goes below the ground grid.

## Mission playback: what is computed, and what is scripted

The **Mission** camera preset, or the Play button, runs the operational sequence in order. The drone and the rover never run at the same time, and they don't coordinate live.

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
| The rover route: Phisiks **A\*** on a navigation grid built from the scene's static colliders (floor, beds, walls), followed by `NavAgent` steering. Move a bed and the route changes. | The drone's flight: a constant-speed spline. There is no flight dynamics and no flight control. |
| The arm pose at every sampling point: analytic **inverse kinematics** (yaw + planar two-link + wrist kept vertical). | Crop "mapping": crop rings are marked when they fall under an assumed 1.2 x 0.9 m footprint. This is not a camera or image-processing model. |
| | Data transfer and processing, and the measurements: timed steps. No sensor values are produced or shown. |

## Panel (left column while the scene is open)

- **Camera:** Presentation, Drone close, Rover close, Overview, Top plan (orthographic, shows the sampling-point IDs), Drone explode, Rover explode, Mission.
- **Mission playback:** play, pause, reset, follow camera, speed, the nine stages with the active one highlighted, and counters for crops mapped and samples taken.
- **Display:** callouts, CAD edges, spin (propellers, LiDAR head, wheels), and explode sliders for the drone and the rover.
- **Subsystems:** visibility boxes and selection for the greenhouse, beds, drone, rover, robotic arm, LiDAR, soil sensors, sampling grid and mission route.
- **Project file:** Save project, Reopen and Rebuild, plus **Screenshot (PNG)**, which writes the 3D view to `screenshots/`.

**Video** is exported as a PNG frame sequence: `PhisiksLab --scene OldMcDonald --export-dir <dir> --export-frames`. Phisiks has no built-in video encoder, so assemble the frames with any external tool.

## Files

| Path | What it is |
|---|---|
| `projects/Old-McDonald/old_mcdonald.scene.json` | the project scene: entities, transforms, components and materials (JSON, `phisiks-scene` v1) |
| `projects/Old-McDonald/project.json` | project metadata (informational) |
| `apps/lab/src/OldMcDonaldSpec.h` | dimensions, placements, sampling grid, arm geometry (shared by the scene and the mesh generator) |
| `apps/lab/src/OldMcDonaldScene.cpp` | builds the project and runs the mission sequence and panel |
| `apps/lab/src/RoverAssembly.cpp` | the rover assembly (parts, hierarchy, materials, explode, arm pose), shared by both scenes |
| `apps/lab/src/DroneAssembly.cpp` | the drone assembly (parts, subsystems, materials, explode, wiring), shared by both scenes |
| `apps/lab/src/CadWorkspace.cpp` | the shared standalone CAD workspace (views, explode, display, subsystems, selection, project file) |
| `apps/lab/src/RoverCadScene.cpp` | the rover CAD workspace (CadWorkspace + the arm-joint controls) |
| `apps/lab/src/DroneCadScene.cpp` | the drone CAD workspace (CadWorkspace) |
| `projects/Old-McDonald/drone_cad.scene.json` | the drone CAD workspace scene file |
| `projects/Old-McDonald/rover_cad.scene.json` | the rover CAD workspace scene file |
| `apps/lab/src/ProjectKit.cpp` | building blocks shared with Project Marina (parts, wiring runs, callouts, visibility, file buttons) |
| `tools/assetgen/OldMcDonaldMeshes.cpp` | generates `assets/meshes/oldmcdonald/*.obj` (CC0, deterministic) |

Saved scenes don't store the navigation components (`NavigationSurface`, `NavAgent`). The scene re-attaches them after every load, and the navigation grid is rebuilt from the saved colliders.

## Verification

`PhisiksLab --autotest` includes two Old-McDonald steps:

- **Project checks:**
  - the hierarchy and named hardware;
  - the 16 sampling points;
  - a render object for every part, and continuous wiring runs;
  - the A* route;
  - per-component picking in the close-up views, and the orthographic top plan showing all 16 points;
  - the exploded views;
  - subsystem visibility;
  - a transform, scale and material edit surviving save and reopen;
  - runtime navigation re-attached after the reopen.
- **Mission checks:**
  - all nine stages run in order;
  - all 48 crops mapped, and the drone landed before the rover left;
  - all 16 points sampled with no A* failures;
  - the rover back within 0.1 m of its dock.
