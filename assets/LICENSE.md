# Phisiks sample assets — CC0 1.0 Universal

Every file in `assets/textures`, `assets/audio`, `assets/meshes` and `assets/models` is generated procedurally by
`tools/assetgen` (no source images, recordings or models are used). The generator's output is
dedicated to the public domain under CC0 1.0 Universal
(https://creativecommons.org/publicdomain/zero/1.0/): you may copy, modify and use it for any
purpose without asking permission.

Regenerate with `Build/bin/Release/assetgen.exe assets` (the output is deterministic).

`assets/meshes/marina` holds the Project Marina parts. They are generic engineering models of
the component classes (single-board computer, UNO-format board, ESP32-C3 board, breadboard,
DC pump, plate heat exchanger, membrane distillation cell, vessels, condenser), sized from
public board outlines where those exist. No manufacturer CAD data or artwork is used.

`assets/meshes/oldmcdonald` holds the Old-McDonald parts (greenhouse, beds and crops, an
F450-class quadcopter with a Pixhawk 2.4.8-size flight-controller case and a GPS puck, and a soil
rover with a Pico-size board, LiDAR, arm and soil probes). These are also generic models sized
from public outline dimensions, with no manufacturer CAD data or artwork.

`assets/scripts/*.lua` are hand-written Phisiks samples, also released under CC0 1.0.
