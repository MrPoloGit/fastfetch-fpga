#include "fpga.h"

const char* ffDetectFPGA([[maybe_unused]] const FFFPGAOptions* options, [[maybe_unused]] FFlist* result) {
    return "Not supported on this platform";
}
