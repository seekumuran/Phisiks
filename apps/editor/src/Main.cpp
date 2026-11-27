// PHISIKS: the general-purpose 3D engine's editor.
//
//   Phisiks [--open <scene.json>] [--gl41] [--quality low|medium|high] [--hot-reload [dir]] ... (--help)
//
// Starts with an empty scene (File > New / Open / Save, Create menu, hierarchy, inspector, gizmo,
// console, telemetry). Assets resolve next to the executable first (release package), then in the
// working directory (repository checkout).
#include "EditorApp.h"

int main(int argc, char** argv) {
    Editor::Launch launch;
    launch.app.windowTitle = "PHISIKS";
    if (const int code = Editor::ParseCommandLine(argc, argv, "Phisiks", launch)) return code == -1 ? 0 : code;
    Editor::Configure(Editor::AppConfig{});
    Editor::Host host;
    return Editor::Run(launch, host);
}
