#pragma once

#include "option.h"

bool ffPrintFPGA(FFFPGAOptions* options);
void ffInitFPGAOptions(FFFPGAOptions* options);
void ffDestroyFPGAOptions(FFFPGAOptions* options);

extern FFModuleBaseInfo ffFPGAModuleInfo;
