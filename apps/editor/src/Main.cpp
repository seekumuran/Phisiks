// PHISIKS: the editor.
//
//   Phisiks [--open <scene.json>] [--project <dir>] [--gl41] [--quality low|medium|high] [--hot-reload [dir]]
//           [--pai [--pai-write] [--pai-simulate]] ... (--help)
//
// Starts with an empty scene (File > New / Open / Save, Create menu, hierarchy, inspector, gizmo,
// console, telemetry). Assets resolve next to the executable first (release package), then in the
// working directory (repository checkout).
#include "EditorApp.h"
#include "Pai.h"

int main(int argc, char** argv) {
    Editor::Launch launch;
    launch.app.windowTitle = "PHISIKS";
    if (const int code = Editor::ParseCommandLine(argc, argv, "Phisiks", launch)) return code == -1 ? 0 : code;
    Editor::Configure(Editor::AppConfig{});
    if (launch.pai) {   // the agent interface: only when the user asks for it, with the permissions they grant
        Pai::Permissions permissions;
        permissions.write = launch.paiWrite;
        permissions.simulate = launch.paiSimulate;
        Pai::StartStdio(permissions);
    }
    Editor::Host host;
    return Editor::Run(launch, host);
}
