#include "build_info.h"
#include "generated/build_time.h"
#include "generated/version.h"

const char *fw_version() { return FW_VERSION; }
const char *fw_built() { return FW_BUILT; }
