#!/usr/bin/env python3
"""Downloads the portable third-party dependencies into vendor/ (pinned versions).

    python tools/fetch_deps.py            # everything that is missing
    python tools/fetch_deps.py --force    # re-download

Fetched: glm, EnTT, stb_image(_write), glad (generated with glad2: GL 4.6 core loader),
Dear ImGui (+ GLFW / OpenGL3 backends), ImGuizmo (+ a compatibility patch for this ImGui
version), miniaudio, nlohmann/json. Not fetched: the Windows prebuilt GLFW and PhysX binaries
(see docs/BUILD.md); Linux/macOS use a system GLFW (or CMake fetches it) and the native physics
backend. Licences: THIRD_PARTY_NOTICES.md.
"""
import io
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDOR = os.environ.get("PHISIKS_VENDOR_DIR") or os.path.join(ROOT, "vendor")
FORCE = "--force" in sys.argv

GLM = "1.0.1"
ENTT = "v3.11.1"
STB = "6b5799bae8a1fe3792691ef4b72ae26434c3e799"          # stb master (stb_image 2.34)
IMGUI = "v1.93.0"
IMGUIZMO = "105e4fde45e4e74aae8973d98750820ef7aca7e2"
MINIAUDIO = "0.11.21"
JSON = "v3.11.3"
GLAD = "2.0.8"
GLAD_EXTENSIONS = "GL_ARB_bindless_texture,GL_ATI_meminfo,GL_EXT_debug_label,GL_EXT_texture_compression_s3tc,GL_KHR_texture_compression_astc_ldr,GL_NVX_gpu_memory_info"


def fetch(url):
    print("  " + url)
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read()


def write(rel, data):
    path = os.path.join(VENDOR, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def need(rel):
    return FORCE or not os.path.exists(os.path.join(VENDOR, rel))


def extract(zip_bytes, prefix_in_zip, dest_rel):
    """Extracts the zip entries below `prefix_in_zip` (after the top-level folder) to vendor/dest_rel."""
    with zipfile.ZipFile(io.BytesIO(zip_bytes)) as z:
        for name in z.namelist():
            parts = name.split("/", 1)
            if len(parts) < 2 or not parts[1].startswith(prefix_in_zip) or name.endswith("/"):
                continue
            rel = parts[1][len(prefix_in_zip):]
            write(os.path.join(dest_rel, rel), z.read(name))


def raw(repo, ref, path):
    return fetch("https://raw.githubusercontent.com/%s/%s/%s" % (repo, ref, path))


def main():
    os.makedirs(VENDOR, exist_ok=True)
    if need("glm/glm/glm.hpp"):
        print("glm " + GLM)
        extract(fetch("https://github.com/g-truc/glm/archive/refs/tags/%s.zip" % GLM), "glm/", "glm/glm")
    if need("entt/entt.hpp"):
        print("EnTT " + ENTT)
        extract(fetch("https://github.com/skypjack/entt/archive/refs/tags/%s.zip" % ENTT), "src/entt/", "entt")
    if need("stb_image/stb_image.h"):
        print("stb " + STB[:10])
        for f in ("stb_image.h", "stb_image_write.h"):
            write("stb_image/" + f, raw("nothings/stb", STB, f))
    if need("imgui/imgui.h"):
        print("Dear ImGui " + IMGUI)
        for f in ("imgui.cpp", "imgui.h", "imgui_internal.h", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
                  "imconfig.h", "imstb_rectpack.h", "imstb_textedit.h", "imstb_truetype.h", "LICENSE.txt",
                  "backends/imgui_impl_glfw.cpp", "backends/imgui_impl_glfw.h", "backends/imgui_impl_opengl3.cpp",
                  "backends/imgui_impl_opengl3.h", "backends/imgui_impl_opengl3_loader.h"):
            write("imgui/" + f, raw("ocornut/imgui", IMGUI, f))
    if need("imguizmo/ImGuizmo.h"):
        print("ImGuizmo " + IMGUIZMO[:10])
        for f in ("src/ImGuizmo.cpp", "src/ImGuizmo.h", "LICENSE"):
            write("imguizmo/" + f.split("/")[-1], raw("CedricGuillemet/ImGuizmo", IMGUIZMO, f))
    patch_imguizmo()
    if need("miniaudio/miniaudio.h"):
        print("miniaudio " + MINIAUDIO)
        for f in ("miniaudio.h", "LICENSE"):
            write("miniaudio/" + f, raw("mackron/miniaudio", MINIAUDIO, f))
    if need("nlohmann/json.hpp"):
        print("nlohmann/json " + JSON)
        write("nlohmann/json.hpp", raw("nlohmann/json", JSON, "single_include/nlohmann/json.hpp"))
        write("nlohmann/LICENSE.MIT", raw("nlohmann/json", JSON, "LICENSE.MIT"))
    if need("glad/src/gl.c"):
        print("glad " + GLAD + " (pip install glad2==%s)" % GLAD)
        subprocess.check_call([sys.executable, "-m", "pip", "install", "--quiet", "glad2==" + GLAD])
        out = os.path.join(VENDOR, "glad")
        shutil.rmtree(out, ignore_errors=True)
        subprocess.check_call([sys.executable, "-m", "glad", "--api", "gl:core=4.6", "--extensions", GLAD_EXTENSIONS,
                               "--out-path", out, "c", "--loader", "--debug"])
    print("vendor/ ready")


def patch_imguizmo():
    """ImGui 1.92.8+ swapped AddPolyline's thickness/flags and removed CaptureMouseFromApp()."""
    path = os.path.join(VENDOR, "imguizmo", "ImGuizmo.cpp")
    with open(path, encoding="utf-8") as f:
        s = f.read()
    original = s
    s = s.replace("colors[3 - axis], false, 2);", "colors[3 - axis], 2.0f, 0);")
    s = s.replace("IM_COL32(0xFF, 0x80, 0x10, 0xFF), true, 2);", "IM_COL32(0xFF, 0x80, 0x10, 0xFF), 2.0f, ImDrawFlags_Closed);")
    s = s.replace("directionColor[i], true, 1.0f);", "directionColor[i], 1.0f, ImDrawFlags_Closed);")
    s = s.replace("ImGui::CaptureMouseFromApp();", "ImGui::SetNextFrameWantCaptureMouse(true);")
    if s != original:
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(s)
        print("  patched ImGuizmo for ImGui " + IMGUI)


if __name__ == "__main__":
    main()
