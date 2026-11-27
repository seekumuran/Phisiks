// projectassetgen: regenerates the CC0 meshes of the example projects (deterministic; outputs are
// committed under projects/assets).
//   projectassetgen [dir]   (default projects/assets)
//     meshes/marina/*.obj       Project Marina parts and the tubing / wiring kit: MarinaMeshes.cpp
//     meshes/oldmcdonald/*.obj  Old-McDonald greenhouse, drone and rover parts: OldMcDonaldMeshes.cpp
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

bool MarinaMeshes(const fs::path& out);        // MarinaMeshes.cpp
bool OldMcDonaldMeshes(const fs::path& out);   // OldMcDonaldMeshes.cpp

int main(int argc, char** argv) {
    const fs::path out = argc > 1 ? fs::path(argv[1]) : fs::path("projects/assets");
    bool ok = MarinaMeshes(out);
    ok &= OldMcDonaldMeshes(out);
    std::printf("projectassetgen: %s -> %s\n", ok ? "ok" : "FAILED", out.string().c_str());
    return ok ? 0 : 1;
}
