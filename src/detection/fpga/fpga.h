#pragma once

#include "fastfetch.h"
#include "modules/fpga/option.h"

#define FF_FPGA_VENDOR_ID_XILINX 0x10EE

#define FF_FPGA_TEMP_UNSET (-DBL_MAX)
#define FF_FPGA_POWER_UNSET (-DBL_MAX)
#define FF_FPGA_PCIE_SPEED_UNSET 0

typedef struct FFFPGAResult {
    FFstrbuf vendor;
    FFstrbuf name;
    FFstrbuf driver; // comma-separated list of drivers bound to this card's PCI functions, empty if none
    FFstrbuf pciAddress; // "domain:bus:device", one entry per physical card, not per PCI function
    uint16_t vendorId;
    uint16_t deviceId;
    double temperature; // FPGA die temperature in Celsius, FF_FPGA_TEMP_UNSET if unavailable
    double power; // total board power in watts, FF_FPGA_POWER_UNSET if unavailable
    uint16_t pcieGen; // max PCIe link generation, FF_FPGA_PCIE_SPEED_UNSET if unavailable
    uint16_t pcieLanes; // max PCIe link width, FF_FPGA_PCIE_SPEED_UNSET if unavailable
} FFFPGAResult;

const char* ffDetectFPGA(const FFFPGAOptions* options, FFlist* result /* list of FFFPGAResult */);
