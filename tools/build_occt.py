#!/usr/bin/env python3
"""Builds the Open CASCADE Technology (OCCT) subset that PHISIKS CAD uses, into vendor/occt.

    python tools/build_occt.py                 # download + build + install (skipped if installed)
    python tools/build_occt.py --force         # rebuild
    python tools/build_occt.py --generator "Visual Studio 17 2022"

Why from source: the official Windows binaries link TKernel against a debug TBB (tbb12_debug.dll)
and jemalloc, and pull FreeType / FreeImage / ffmpeg / OpenVR into the data-exchange toolkits. This
build uses the native memory manager and no third-party products at all, so the shipped libraries
are OCCT itself (LGPL-2.1 with the OCCT exception, linked dynamically) and the system runtime.

Modules: FoundationClasses, ModelingData, ModelingAlgorithms, DataExchange (STEP, IGES, STL).
DataExchange needs TKXCAF, which needs TKV3d / TKService; they are built without FreeType, X11 or
OpenGL. Not built: Visualization (TKOpenGl), ApplicationFramework UI, Draw, DETools, samples.

Result: vendor/occt/{include/opencascade, lib, bin (Windows DLLs), cmake config, licences}.
PHISIKS finds it with -DPHISIKS_OCCT_DIR=vendor/occt (the default when it exists).
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION = "7.9.3"
TAG = "V7_9_3"
URL = f"https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/{TAG}.tar.gz"
SHA256 = "5ecf094ec6b12d5413dfb851d8c3590c354058aee556e32e408bdfbf8c357d57"


def arg(name, default=None):
    if name in sys.argv:
        i = sys.argv.index(name)
        return sys.argv[i + 1] if i + 1 < len(sys.argv) else default
    return default


PREFIX = os.path.abspath(arg("--prefix", os.path.join(ROOT, "vendor", "occt")))
WORK = os.path.abspath(arg("--work", os.path.join(ROOT, "Build", "occt")))
GENERATOR = arg("--generator")
FORCE = "--force" in sys.argv
STAMP = os.path.join(PREFIX, "PHISIKS_OCCT_BUILD.txt")


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.check_call(cmd, **kw)


def main():
    if not FORCE and os.path.exists(STAMP) and open(STAMP).read().startswith(VERSION):
        print(f"OCCT {VERSION} already installed in {PREFIX} (--force to rebuild)")
        return
    os.makedirs(WORK, exist_ok=True)
    archive = os.path.join(WORK, f"occt-{TAG}.tar.gz")
    if not os.path.exists(archive) or hashlib.sha256(open(archive, "rb").read()).hexdigest() != SHA256:
        print("downloading " + URL, flush=True)
        with urllib.request.urlopen(URL, timeout=600) as r:
            data = r.read()
        digest = hashlib.sha256(data).hexdigest()
        if digest != SHA256:
            sys.exit(f"OCCT archive checksum mismatch: {digest} (expected {SHA256})")
        with open(archive, "wb") as f:
            f.write(data)
    src = os.path.join(WORK, f"OCCT-{TAG.replace('V', '', 1)}")
    if not os.path.isdir(src):
        with tarfile.open(archive) as t:
            t.extractall(WORK, filter="data") if sys.version_info >= (3, 12) else t.extractall(WORK)
    build = os.path.join(WORK, "build")
    if FORCE and os.path.isdir(build):
        shutil.rmtree(build)
    options = {
        "BUILD_LIBRARY_TYPE": "Shared",
        "CMAKE_BUILD_TYPE": "Release",
        "INSTALL_DIR": PREFIX,
        "INSTALL_DIR_LAYOUT": "Unix",            # include/opencascade, lib, bin on every platform
        "BUILD_MODULE_FoundationClasses": "ON",
        "BUILD_MODULE_ModelingData": "ON",
        "BUILD_MODULE_ModelingAlgorithms": "ON",
        "BUILD_MODULE_DataExchange": "ON",
        "BUILD_MODULE_Visualization": "OFF",
        "BUILD_MODULE_ApplicationFramework": "OFF",
        "BUILD_MODULE_Draw": "OFF",
        "BUILD_MODULE_DETools": "OFF",
        "BUILD_DOC_Overview": "OFF",
        "BUILD_SAMPLES_QT": "OFF",
        "BUILD_SAMPLES_MFC": "OFF",
        "BUILD_USE_PCH": "OFF",
        "USE_FREETYPE": "OFF", "USE_FREEIMAGE": "OFF", "USE_FFMPEG": "OFF", "USE_OPENVR": "OFF",
        "USE_RAPIDJSON": "OFF", "USE_DRACO": "OFF", "USE_TBB": "OFF", "USE_VTK": "OFF",
        "USE_TK": "OFF", "USE_XLIB": "OFF", "USE_OPENGL": "OFF", "USE_GLES2": "OFF", "USE_D3D": "OFF",
        "USE_MMGR_TYPE": "NATIVE",
        # next to each other (PHISIKS copies them beside its programs) or in prefix/lib
        "CMAKE_INSTALL_RPATH": "@loader_path;@loader_path/../lib" if sys.platform == "darwin" else "$ORIGIN;$ORIGIN/../lib",
    }
    cmd = ["cmake", "-S", src, "-B", build] + [f"-D{k}={v}" for k, v in options.items()]
    if GENERATOR:
        cmd += ["-G", GENERATOR]
    if os.name == "nt" and (not GENERATOR or GENERATOR.startswith("Visual Studio")):
        cmd += ["-A", "x64"]
    run(cmd)
    jobs = str(os.cpu_count() or 4)
    run(["cmake", "--build", build, "--config", "Release", "--parallel", jobs])
    run(["cmake", "--install", build, "--config", "Release"])
    for lic in ("LICENSE_LGPL_21.txt", "OCCT_LGPL_EXCEPTION.txt"):
        shutil.copy(os.path.join(src, lic), os.path.join(PREFIX, lic))
    with open(STAMP, "w") as f:
        f.write(f"{VERSION}\nsource {URL}\nsha256 {SHA256}\noptions {options}\n")
    print(f"OCCT {VERSION} installed in {PREFIX}")


if __name__ == "__main__":
    main()
