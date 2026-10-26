#pragma once

// From scripts/version.py, in the one file that is recompiled every build
const char *fw_version(); // git describe, e.g. v1.2.0-3-gabc1234-dirty
const char *fw_built();   // build time: differs between any two builds
