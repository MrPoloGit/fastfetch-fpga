#include "gpu.h"
#include "common/debug.h"
#include "common/pciid.h"
#include "common/properties.h"
#include "common/strutil.h"

#if FF_HAVE_EMBEDDED_AMDGPUIDS
    #include "fastfetch_amdgpuids.c.inc"
#endif

void ffGPUParsePciIdsFile(const FFstrbuf* content, uint8_t subclass, uint16_t vendor, uint16_t device, FFGPUResult* gpu) {
    FF_DEBUG("Searching pci.ids for vendor=0x%04x device=0x%04x subclass=0x%02x", vendor, device, subclass);
    ffPciIdsLookupFile(content, vendor, device, &gpu->vendor, &gpu->name);

    if (!gpu->name.length) {
        const char* subclassStr;
        switch (subclass) {
            case 0 /*PCI_CLASS_DISPLAY_VGA*/:
                subclassStr = " (VGA compatible)";
                break;
            case 1 /*PCI_CLASS_DISPLAY_XGA*/:
                subclassStr = " (XGA compatible)";
                break;
            case 2 /*PCI_CLASS_DISPLAY_3D*/:
                subclassStr = " (3D)";
                break;
            default:
                subclassStr = "";
                break;
        }

        ffStrbufSetF(&gpu->name, "%s Device %04X%s", gpu->vendor.length ? gpu->vendor.chars : "Unknown", device, subclassStr);
        FF_DEBUG("PCI device name not found; synthesized name: %s", gpu->name.chars);
    } else {
        FF_DEBUG("Resolved PCI device name: vendor='%s', name='%s'", gpu->vendor.chars, gpu->name.chars);
    }
}

void ffGPUFillVendorAndName(uint8_t subclass, uint16_t vendor, uint16_t device, FFGPUResult* gpu) {
    FF_DEBUG("Resolving PCI GPU name: vendor=0x%04x device=0x%04x subclass=0x%02x", vendor, device, subclass);
    if (vendor == 0x1234 && device == 0x1111 && subclass == 0) { // Not exist in pci.ids
        ffStrbufSetStatic(&gpu->name, "Virtual Video Controller");
        FF_DEBUG("Matched special virtual video controller ID");
        return;
    }

#if FF_HAVE_EMBEDDED_PCIIDS
    bool ok = ffPciIdsLookupEmbedded(vendor, device, &gpu->vendor, &gpu->name);
    if (ok) {
        if (!gpu->name.length) {
            const char* subclassStr;
            switch (subclass) {
                case 0 /*PCI_CLASS_DISPLAY_VGA*/:
                    subclassStr = " (VGA compatible)";
                    break;
                case 1 /*PCI_CLASS_DISPLAY_XGA*/:
                    subclassStr = " (XGA compatible)";
                    break;
                case 2 /*PCI_CLASS_DISPLAY_3D*/:
                    subclassStr = " (3D)";
                    break;
                default:
                    subclassStr = "";
                    break;
            }
            ffStrbufSetF(&gpu->name, "%s Device %04X%s", gpu->vendor.length ? gpu->vendor.chars : "Unknown", device, subclassStr);
        }
        FF_DEBUG("Resolved PCI GPU name from embedded IDs: vendor='%s', name='%s'", gpu->vendor.chars, gpu->name.chars);
        return;
    }
#endif
    ffGPUParsePciIdsFile(ffPciIdsLoadFile(), subclass, vendor, device, gpu);
    FF_DEBUG("Resolved PCI GPU name from pci.ids: vendor='%s', name='%s'", gpu->vendor.chars, gpu->name.chars);
}

#if FF_HAVE_EMBEDDED_AMDGPUIDS
static inline int amdGpuCmp(const uint32_t* key, const FFArmGpuProduct* element) {
    // Maximum value of *key is 0x00FFFFFF. `(int) *key` should never overflow
    return (int) *key - (int) element->id;
}

static bool loadAmdGpuIdsInc(uint16_t deviceId, uint8_t revision, FFGPUResult* gpu) {
    uint32_t key = (deviceId << 8u) | revision;
    FFArmGpuProduct* product = bsearch(&key, ffAmdGpuProducts, ARRAY_SIZE(ffAmdGpuProducts), sizeof(*ffAmdGpuProducts), (void*) amdGpuCmp);
    if (product) {
        ffStrbufSetS(&gpu->name, product->name);
        FF_DEBUG("Embedded AMD GPU ID matched: device=0x%04x revision=0x%02x name='%s'", deviceId, revision, gpu->name.chars);
        return true;
    }
    FF_DEBUG("Embedded AMD GPU ID not found: device=0x%04x revision=0x%02x", deviceId, revision);
    return false;
}
#endif

static void parseAmdGpuIdsFile(uint16_t deviceId, uint8_t revision, FFGPUResult* gpu) {
    char query[32];
    snprintf(query, ARRAY_SIZE(query), "%04X,\t%02X,", (unsigned) deviceId, (unsigned) revision);
#ifdef FF_CUSTOM_AMDGPU_IDS_PATH
    FF_DEBUG("Searching AMD GPU IDs at custom path '%s' for device=0x%04x revision=0x%02x", FF_STR(FF_CUSTOM_AMDGPU_IDS_PATH), deviceId, revision);
    ffParsePropFile(FF_STR(FF_CUSTOM_AMDGPU_IDS_PATH), query, &gpu->name);
#else
    FF_DEBUG("Searching libdrm/amdgpu.ids for device=0x%04x revision=0x%02x", deviceId, revision);
    ffParsePropFileData("libdrm/amdgpu.ids", query, &gpu->name);
#endif
    FF_DEBUG("AMD GPU IDs file lookup %s: device=0x%04x revision=0x%02x name='%s'", gpu->name.length ? "matched" : "not matched", deviceId, revision, gpu->name.chars);
}

void ffGPUQueryAmdGpuName(uint16_t deviceId, uint8_t revisionId, FFGPUResult* gpu) {
    FF_DEBUG("Resolving AMD GPU name: device=0x%04x revision=0x%02x", deviceId, revisionId);
#if FF_HAVE_EMBEDDED_AMDGPUIDS
    bool ok = loadAmdGpuIdsInc(deviceId, revisionId, gpu);
    if (ok) {
        return;
    }
#endif
    parseAmdGpuIdsFile(deviceId, revisionId, gpu);
}
