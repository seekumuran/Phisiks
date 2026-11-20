#pragma once
// Installs the scene presentation backends: Phisiks::Render for meshes/lights/cameras (omitted in
// headless runs) and Phisiks::Audio for AudioSource playback.
namespace Phisiks::Presentation {
    void Install(bool withRenderer);
    void Uninstall();
}
