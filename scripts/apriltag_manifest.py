# PlatformIO pre-build hook. apriltag v3.3.0 ships no library.json, so on
# install PlatformIO generates a bare one and would compile every .c file —
# including apriltag_pywrap.c, which needs Python.h. Add a source filter that
# builds only the C library, and let common/pjpeg-idct.c pass int32_t* as int*
# (same width on Xtensa; GCC 14 made that mismatch an error).
import json
import os

Import("env")  # noqa: F821 — provided by PlatformIO/SCons

BUILD = {
    "srcFilter": "+<*.c> -<apriltag_pywrap.c> +<common/*.c>",
    "flags": "-Wno-error=incompatible-pointer-types",
}

lib_dir = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "apriltag")
manifest_path = os.path.join(lib_dir, "library.json")

if os.path.isdir(lib_dir):
    manifest = {"name": "apriltag", "version": "3.3.0"}
    if os.path.exists(manifest_path):
        with open(manifest_path) as f:
            manifest = json.load(f)
    if manifest.get("build") != BUILD:
        manifest["build"] = BUILD
        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=2)
        print("apriltag_manifest: patched " + manifest_path)
