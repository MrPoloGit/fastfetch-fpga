#pragma once

#include "common/option.h"
#include "common/parsing.h"

typedef struct FFFPGAOptions {
    FFModuleArgs moduleArgs;
    bool temp; // also gates power, since both come from the same hwmon scan
    FFColorRangeConfig tempConfig;
} FFFPGAOptions;

static_assert(sizeof(FFFPGAOptions) <= FF_OPTION_MAX_SIZE, "FFFPGAOptions size exceeds maximum allowed size");
