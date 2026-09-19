# PlatformIO pre-build hook. apriltag v3.3.0 ships no library.json, so on
# install PlatformIO generates a bare one and would compile every .c file —
# including apriltag_pywrap.c, which needs Python.h. Add a source filter that
# builds only the C library, and let common/pjpeg-idct.c pass int32_t* as int*
# (same width on Xtensa; GCC 14 made that mismatch an error).
import json
import re
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



# The ESP32-S3 FPU is single precision; every double operation is a libgcc
# soft-float call. Quad fitting (apriltag_quad_thresh.c, line fits for every
# candidate blob) and refine_edges() (apriltag.c, ~16k samples per quad) are
# almost all double maths and dominated detection time. Rewrite them in float:
# `double` → float, math calls → their f versions, float literals get an f
# suffix. Neither passes doubles to other files. Measured on the board: quad
# fitting 4-6× faster; host build: corners within 0.01 px of the double code.
MARK = "/* patched: scripts/apriltag_manifest.py */\n"
FLOAT_REGIONS = {  # file: (start of the region to convert, end of it)
    "apriltag_quad_thresh.c": ('#include "common/math_util.h"\n', None),
    "apriltag.c": ("static void refine_edges(", "static void quad_decode_task("),
}
# Pose: fix_pose_ambiguities() root-finds a quartic, evaluating it with a
# software-double pow() per term; Horner's rule gives the same polynomial.
REPLACE = {
    "apriltag_pose.c": [
        (
            "    for (int i = 0; i <= degree; i++) {\n"
            "        ret += p[i]*pow(x, i);\n"
            "    }\n",
            "    for (int i = degree; i >= 0; i--) {\n"
            "        ret = ret*x + p[i];\n"
            "    }\n",
        )
    ],
}
_LITERAL = re.compile(r"(?<![\w.%])((?:\d+\.\d*|\.\d+)(?:[eE][+-]?\d+)?|\d+[eE][+-]?\d+)(?![\w.])")
_MATH = re.compile(r"\b(sqrt|log|exp|fmin|fmax|fabs|floor|ceil|cos|sin|atan2)\(")
# comments and string literals pass through untouched
_SKIP = re.compile(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/', re.S)


def _code_to_float(code):
    code = re.sub(r"\bdouble\b", "float", code)
    code = _MATH.sub(r"\1f(", code)
    return _LITERAL.sub(r"\1f", code)


def to_single_precision(src, start, end):
    a = src.index(start)
    b = src.index(end, a) if end else len(src)
    region, out, pos = src[a:b], [], 0
    for m in _SKIP.finditer(region):
        out.append(_code_to_float(region[pos : m.start()]))
        out.append(m.group(0))
        pos = m.end()
    out.append(_code_to_float(region[pos:]))
    return src[:a] + "".join(out) + src[b:]


for name in sorted(set(FLOAT_REGIONS) | set(REPLACE)):
    path = os.path.join(lib_dir, name)
    if not os.path.exists(path):
        continue
    with open(path) as f:
        src = f.read()
    if src.startswith(MARK):
        continue
    if name in FLOAT_REGIONS:
        src = to_single_precision(src, *FLOAT_REGIONS[name])
    for old, new in REPLACE.get(name, []):
        if old not in src:  # apriltag changed: fail loudly, not silently slow
            raise RuntimeError("apriltag_manifest: patch no longer applies to " + path)
        src = src.replace(old, new)
    with open(path, "w") as f:
        f.write(MARK + src)
    print("apriltag_manifest: patched " + path)
