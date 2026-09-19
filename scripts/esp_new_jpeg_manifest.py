# PlatformIO pre-build hook. esp_new_jpeg (Espressif's SIMD JPEG codec) is an
# ESP-IDF component: headers plus a prebuilt static library per chip, no
# PlatformIO manifest. Point the build at the headers and link the ESP32-S3
# archive; its test app is not built.
import json
import os

Import("env")  # noqa: F821 — provided by PlatformIO/SCons

lib_dir = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "esp_new_jpeg")
manifest_path = os.path.join(lib_dir, "library.json")
BUILD = {
    "srcFilter": "-<*>",
    "includeDir": "include",
    "flags": ["-L" + os.path.join(lib_dir, "lib", "esp32s3"), "-lesp_new_jpeg"],
}

if os.path.isdir(lib_dir):
    manifest = {"name": "esp_new_jpeg", "version": "1.0.2"}
    if os.path.exists(manifest_path):
        with open(manifest_path) as f:
            manifest = json.load(f)
    if manifest.get("build") != BUILD:
        manifest["build"] = BUILD
        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=2)
        print("esp_new_jpeg_manifest: patched " + manifest_path)
