#include "detection/fpga/fpga.h"
#include "common/debug.h"
#include "common/io.h"
#include "common/pciid.h"
#include "common/FFstrbuf.h"

#include <inttypes.h>
#include <stdint.h>

// Alveo/Versal device ids pci.ids doesn't list yet. Only used as a fallback after the pci.ids
// lookup below comes up empty - safe to delete an entry once pci.ids adds it.
//
// Each shell exposes up to 4 PCI functions (PF0-PF3) at consecutive device ids; the base (PF0) id
// is listed here, and `fpgaDeviceIdToFallbackModel` masks off the low 2 bits to match any of them.
typedef struct FFFPGADeviceIdEntry {
    uint16_t deviceId; // PF0 id, or the golden image id
    const char* model;
} FFFPGADeviceIdEntry;

static const FFFPGADeviceIdEntry FF_FPGA_DEVICE_IDS[] = {
    { 0x5010, "Alveo U200" }, // QDMA shell
    { 0x5014, "Alveo U250" }, // QDMA shell
    { 0x5044, "Versal VCK5000" },
    { 0x5048, "Versal VCK5000" }, // QDMA shell
    { 0x5058, "Alveo U55N" },
    { 0x5060, "Alveo U50LV" },
    { 0x506C, "Alveo U50C" },
    { 0x5070, "MA35D" },
    { 0x5094, "Alveo V70" },
    { 0x50B0, "Alveo V70" }, // PQ2 QDMA shell
    { 0x50C0, "UL3xxx" },
    { 0x513C, "Alveo U30" },
    // Golden (factory recovery) image
    { 0xD03C, "Alveo U30 (Golden image)" },
};

static const char* fpgaDeviceIdToFallbackModel(uint16_t deviceId) {
    uint16_t base = (uint16_t) (deviceId & ~0x3);
    for (uint32_t i = 0; i < ARRAY_SIZE(FF_FPGA_DEVICE_IDS); ++i) {
        if (FF_FPGA_DEVICE_IDS[i].deviceId == deviceId || FF_FPGA_DEVICE_IDS[i].deviceId == base) {
            return FF_FPGA_DEVICE_IDS[i].model;
        }
    }
    return nullptr;
}

// Resolves `deviceId` to a vendor/model name: pci.ids first (embedded or system file, same as
// `ffGPUFillVendorAndName` does for `gpu`), then the manual table above, then a synthesized name.
static void fpgaResolveVendorAndName(uint16_t deviceId, FFstrbuf* vendor, FFstrbuf* name) {
    FF_DEBUG("Resolving PCI FPGA name: vendor=0x%04x device=0x%04x", FF_FPGA_VENDOR_ID_XILINX, deviceId);

#if FF_HAVE_EMBEDDED_PCIIDS
    if (ffPciIdsLookupEmbedded(FF_FPGA_VENDOR_ID_XILINX, deviceId, vendor, name)) {
        FF_DEBUG("Resolved PCI FPGA name from embedded IDs: vendor='%s', name='%s'", vendor->chars, name->chars);
    }
#else
    ffPciIdsLookupFile(ffPciIdsLoadFile(), FF_FPGA_VENDOR_ID_XILINX, deviceId, vendor, name);
    FF_DEBUG("Resolved PCI FPGA name from pci.ids: vendor='%s', name='%s'", vendor->chars, name->chars);
#endif

    if (!name->length) {
        const char* fallbackModel = fpgaDeviceIdToFallbackModel(deviceId);
        if (fallbackModel) {
            FF_DEBUG("pci.ids had no entry for device 0x%04x; using manual fallback table: %s", deviceId, fallbackModel);
            ffStrbufSetS(name, fallbackModel);
        }
    }

    if (!vendor->length) {
        ffStrbufSetStatic(vendor, "Xilinx");
    }

    if (!name->length) {
        ffStrbufSetF(name, "%s Device %04X", vendor->chars, deviceId);
        FF_DEBUG("No match in pci.ids or the manual table; synthesized name: %s", name->chars);
    }
}

// A physical card can expose several PCI functions (management + user, ...); this is one entry
// per physical slot (domain:bus:device), merging every function's driver into `driver`.
typedef struct FFFPGASlot {
    uint32_t domain, bus, device;
    uint16_t vendorId, deviceId; // identity used for naming, taken from the lowest-numbered function seen
    bool hasId;
    FFstrbuf driver;
    double temperature;
    double power;
    uint16_t pcieGen;
    uint16_t pcieLanes;
} FFFPGASlot;

static FFFPGASlot* findOrAddSlot(FFlist* slots, uint32_t domain, uint32_t bus, uint32_t device) {
    FF_LIST_FOR_EACH (FFFPGASlot, slot, *slots) {
        if (slot->domain == domain && slot->bus == bus && slot->device == device) {
            return slot;
        }
    }

    FFFPGASlot* slot = FF_LIST_ADD(FFFPGASlot, *slots);
    slot->domain = domain;
    slot->bus = bus;
    slot->device = device;
    slot->vendorId = 0;
    slot->deviceId = 0;
    slot->hasId = false;
    ffStrbufInit(&slot->driver);
    slot->temperature = FF_FPGA_TEMP_UNSET;
    slot->power = FF_FPGA_POWER_UNSET;
    slot->pcieGen = FF_FPGA_PCIE_SPEED_UNSET;
    slot->pcieLanes = FF_FPGA_PCIE_SPEED_UNSET;
    return slot;
}

// Finds the hwmon channel whose `*_label` file contains `label` (case-insensitive) and returns its
// `*_input` value divided by `divisor` (e.g. 1000 for millidegrees/millivolts). Channel numbering
// isn't guaranteed stable across driver versions, so this matches on the label, not a fixed index.
static double findLabeledSensor(FFstrbuf* hwmonDir, FFstrbuf* buffer, const char* prefix, const char* label, double divisor) {
    const uint32_t hwmonDirLen = hwmonDir->length;
    for (uint32_t i = 1; i <= 32; ++i) {
        ffStrbufAppendF(hwmonDir, "%s%u_label", prefix, i);
        bool matched = ffReadFileBuffer(hwmonDir->chars, buffer) && ffStrbufContainIgnCaseS(buffer, label);
        ffStrbufSubstrBefore(hwmonDir, hwmonDirLen);
        if (!matched) {
            continue;
        }

        ffStrbufAppendF(hwmonDir, "%s%u_input", prefix, i);
        double value = FF_FPGA_TEMP_UNSET;
        if (ffReadFileBuffer(hwmonDir->chars, buffer)) {
            value = (double) ffStrbufToUInt(buffer, 0) / divisor;
        }
        ffStrbufSubstrBefore(hwmonDir, hwmonDirLen);
        return value;
    }
    return FF_FPGA_TEMP_UNSET;
}

// Reads the FPGA die temperature and total board power from whichever hwmon device is exposed under
// `pciDir` (either the management or user PCI function works; both report the same sensors).
static void detectSensors(FFstrbuf* pciDir, FFstrbuf* buffer, double* temperature, double* power) {
    const uint32_t pciDirLen = pciDir->length;
    ffStrbufAppendS(pciDir, "/hwmon/");
    FF_AUTO_CLOSE_DIR DIR* dirp = opendir(pciDir->chars);
    if (!dirp) {
        ffStrbufSubstrBefore(pciDir, pciDirLen);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dirp)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        ffStrbufAppendS(pciDir, entry->d_name);
        ffStrbufAppendC(pciDir, '/');
        const uint32_t hwmonDirLen = pciDir->length;

        *temperature = findLabeledSensor(pciDir, buffer, "temp", "FPGA TEMP", 1000.0);

        // No `*_label` file for power channels on this driver; `power1_input` is total board power
        // in microwatts, confirmed against real U250 hwmon data (`power2_input` reads 0, unused).
        ffStrbufAppendS(pciDir, "power1_input");
        if (ffReadFileBuffer(pciDir->chars, buffer)) {
            *power = (double) ffStrbufToUInt(buffer, 0) / 1000000.0;
        }
        ffStrbufSubstrBefore(pciDir, hwmonDirLen);
        break;
    }

    ffStrbufSubstrBefore(pciDir, pciDirLen);
}

// Standard kernel PCI sysfs files, same parsing as the `gpu` module uses.
static inline uint16_t pcieLinkSpeedToGen(const FFstrbuf* buffer) {
    int64_t speed = ffStrbufToSInt(buffer, FF_FPGA_PCIE_SPEED_UNSET);
    if (speed >= 64) {
        return 6;
    } else if (speed >= 32) {
        return 5;
    } else if (speed >= 16) {
        return 4;
    } else if (speed >= 8) {
        return 3;
    } else if (speed >= 5) {
        return 2;
    } else if (speed >= 2) { // 2.5
        return 1;
    }
    return FF_FPGA_PCIE_SPEED_UNSET;
}

static inline uint16_t pcieWidthToLanes(const FFstrbuf* buffer) {
    int64_t width = ffStrbufToSInt(buffer, FF_FPGA_PCIE_SPEED_UNSET);
    if (width > 0 && width < 255) { // kernel returns 255 if the value is unknown
        return (uint16_t) width;
    }
    return FF_FPGA_PCIE_SPEED_UNSET;
}

// Reads the card's max PCIe link speed/width from `pciDir`.
static void detectPcieSpeed(FFstrbuf* pciDir, FFstrbuf* buffer, uint16_t* pcieGen, uint16_t* pcieLanes) {
    const uint32_t pciDirLen = pciDir->length;

    ffStrbufAppendS(pciDir, "/max_link_speed");
    if (ffReadFileBuffer(pciDir->chars, buffer)) {
        *pcieGen = pcieLinkSpeedToGen(buffer);
    }
    ffStrbufSubstrBefore(pciDir, pciDirLen);

    if (*pcieGen != FF_FPGA_PCIE_SPEED_UNSET) {
        ffStrbufAppendS(pciDir, "/max_link_width");
        if (ffReadFileBuffer(pciDir->chars, buffer)) {
            *pcieLanes = pcieWidthToLanes(buffer);
        }
        ffStrbufSubstrBefore(pciDir, pciDirLen);
    }
}

// Appends the driver bound to the PCI device at `pciDir` (e.g. `xclmgmt`) to `result`.
static bool detectDriverFromSysfs(FFstrbuf* result, FFstrbuf* pciDir) {
    uint32_t pciDirLength = pciDir->length;
    ffStrbufAppendS(pciDir, "/driver");
    char pathBuf[PATH_MAX];
    ssize_t resultLength = readlink(pciDir->chars, pathBuf, ARRAY_SIZE(pathBuf));
    ffStrbufSubstrBefore(pciDir, pciDirLength);
    if (resultLength <= 0) {
        return false;
    }

    const char* slash = memrchr(pathBuf, '/', (size_t) resultLength);
    if (slash) {
        slash++;
        ffStrbufAppendNS(result, (uint32_t) (resultLength - (slash - pathBuf)), slash);
    } else {
        ffStrbufAppendNS(result, (uint32_t) resultLength, pathBuf);
    }
    return true;
}

const char* ffDetectFPGA(const FFFPGAOptions* options, FFlist* result) {
    const char* pciDirPath = "/sys/bus/pci/devices/";

    FF_AUTO_CLOSE_DIR DIR* dirp = opendir(pciDirPath);
    if (dirp == nullptr) {
        FF_DEBUG("opendir(%s) failed: %s", pciDirPath, strerror(errno));
        return "Failed to open `/sys/bus/pci/devices/`";
    }

    FF_STRBUF_AUTO_DESTROY pciDir = ffStrbufCreateA(64);
    ffStrbufAppendS(&pciDir, pciDirPath);
    const uint32_t pciBaseDirLength = pciDir.length;

    FF_STRBUF_AUTO_DESTROY buffer = ffStrbufCreate();
    FF_LIST_AUTO_DESTROY slots = ffListCreate();

    struct dirent* entry;
    while ((entry = readdir(dirp)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        ffStrbufSubstrBefore(&pciDir, pciBaseDirLength);
        ffStrbufAppendS(&pciDir, entry->d_name);
        const uint32_t pciDevDirLength = pciDir.length;

        ffStrbufAppendS(&pciDir, "/modalias");
        if (!ffReadFileBuffer(pciDir.chars, &buffer)) {
            continue;
        }
        ffStrbufSubstrBefore(&pciDir, pciDevDirLength);
        if (!ffStrbufStartsWithS(&buffer, "pci:")) {
            continue;
        }

        uint32_t vendorId, deviceId, subVendorId, subDeviceId;
        uint8_t classId, subclassId;
        if (sscanf(buffer.chars + strlen("pci:"), "v%8" SCNx32 "d%8" SCNx32 "sv%8" SCNx32 "sd%8" SCNx32 "bc%2" SCNx8 "sc%2" SCNx8, &vendorId, &deviceId, &subVendorId, &subDeviceId, &classId, &subclassId) != 6) {
            continue;
        }

        if (vendorId != FF_FPGA_VENDOR_ID_XILINX) {
            continue;
        }

        // Xilinx also makes plain PCIe bridge chips under the same vendor ID (e.g. `10ee:9134`,
        // class [0604]); only `0x12` (Processing accelerators) is an actual accelerator card.
        if (classId != 0x12) {
            continue;
        }

        uint32_t pciDomain, pciBus, pciDevice, pciFunc;
        if (sscanf(entry->d_name, "%" SCNx32 ":%" SCNx32 ":%" SCNx32 ".%" SCNx32, &pciDomain, &pciBus, &pciDevice, &pciFunc) != 4) {
            FF_DEBUG("Invalid PCI device path: %s", entry->d_name);
            continue;
        }

        FFFPGASlot* slot = findOrAddSlot(&slots, pciDomain, pciBus, pciDevice);
        if (!slot->hasId || pciFunc == 0) {
            slot->vendorId = (uint16_t) vendorId;
            slot->deviceId = (uint16_t) deviceId;
            slot->hasId = true;

            if (options->temp) {
                detectSensors(&pciDir, &buffer, &slot->temperature, &slot->power);
            }
            detectPcieSpeed(&pciDir, &buffer, &slot->pcieGen, &slot->pcieLanes);
        }

        if (slot->driver.length) {
            ffStrbufAppendS(&slot->driver, ", ");
        }
        detectDriverFromSysfs(&slot->driver, &pciDir);
    }

    FF_LIST_FOR_EACH (FFFPGASlot, slot, slots) {
        FFFPGAResult* fpga = FF_LIST_ADD(FFFPGAResult, *result);
        ffStrbufInit(&fpga->vendor);
        ffStrbufInit(&fpga->name);
        ffStrbufInitMove(&fpga->driver, &slot->driver);
        ffStrbufInitF(&fpga->pciAddress, "%04x:%02x:%02x", slot->domain, slot->bus, slot->device);
        fpga->vendorId = slot->vendorId;
        fpga->deviceId = slot->deviceId;
        fpga->temperature = slot->temperature;
        fpga->power = slot->power;
        fpga->pcieGen = slot->pcieGen;
        fpga->pcieLanes = slot->pcieLanes;

        fpgaResolveVendorAndName(slot->deviceId, &fpga->vendor, &fpga->name);
    }

    return nullptr;
}
