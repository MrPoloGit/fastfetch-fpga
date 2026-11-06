#include "common/pciid.h"
#include "common/debug.h"
#include "common/io.h"
#include "common/path.h"
#include "common/memrchr.h"

#include <stdio.h>
#include <string.h>

#if FF_HAVE_EMBEDDED_PCIIDS
    #include "fastfetch_pciids.c.inc"
#endif

const FFstrbuf* ffPciIdsLoadFile() {
    static FFstrbuf pciids;

    if (pciids.chars) {
        return &pciids;
    }
    ffStrbufInit(&pciids);

#ifdef FF_CUSTOM_PCI_IDS_PATH

    FF_DEBUG("Loading PCI IDs from custom path: %s", FF_STR(FF_CUSTOM_PCI_IDS_PATH));
    ffReadFileBuffer(FF_STR(FF_CUSTOM_PCI_IDS_PATH), &pciids);

#else // FF_CUSTOM_PCI_IDS_PATH

    #if __linux__
    FF_DEBUG("Loading PCI IDs from %s", FASTFETCH_TARGET_DIR_USR "/share/hwdata/pci.ids");
    ffReadFileBuffer(FASTFETCH_TARGET_DIR_USR "/share/hwdata/pci.ids", &pciids);
    if (pciids.length == 0) {
        FF_DEBUG("PCI IDs not found, trying %s", FASTFETCH_TARGET_DIR_USR "/share/misc/pci.ids");
        ffReadFileBuffer(FASTFETCH_TARGET_DIR_USR "/share/misc/pci.ids", &pciids); // debian?
        if (pciids.length == 0) {
            FF_DEBUG("PCI IDs not found, trying %s", FASTFETCH_TARGET_DIR_USR "/local/share/hwdata/pci.ids");
            ffReadFileBuffer(FASTFETCH_TARGET_DIR_USR "/local/share/hwdata/pci.ids", &pciids);
        }
    }
    #elif __OpenBSD__ || __FreeBSD__ || __NetBSD__
    FF_DEBUG("Loading PCI IDs from %s", FF_PATH_PKG_BASE "/share/hwdata/pci.ids");
    ffReadFileBuffer(FF_PATH_PKG_BASE "/share/hwdata/pci.ids", &pciids);
    if (pciids.length == 0) {
        FF_DEBUG("PCI IDs not found, trying %s", FF_PATH_PKG_BASE "/share/pciids/pci.ids");
        ffReadFileBuffer(FF_PATH_PKG_BASE "/share/pciids/pci.ids", &pciids);
    }
    #elif __sun
    FF_DEBUG("Loading PCI IDs from %s", FASTFETCH_TARGET_DIR_ROOT "/usr/share/hwdata/pci.ids");
    ffReadFileBuffer(FASTFETCH_TARGET_DIR_ROOT "/usr/share/hwdata/pci.ids", &pciids);
    #elif __HAIKU__
    FF_DEBUG("Loading PCI IDs from %s", FASTFETCH_TARGET_DIR_ROOT "/system/data/hwdata/pci.ids");
    ffReadFileBuffer(FASTFETCH_TARGET_DIR_ROOT "/system/data/hwdata/pci.ids", &pciids);
    #endif

#endif // FF_CUSTOM_PCI_IDS_PATH

    FF_DEBUG("PCI IDs data loaded: %u bytes", pciids.length);
    return &pciids;
}

void ffPciIdsLookupFile(const FFstrbuf* content, uint16_t vendor, uint16_t device, FFstrbuf* vendorName, FFstrbuf* deviceName) {
    FF_DEBUG("Searching pci.ids for vendor=0x%04x device=0x%04x", vendor, device);
    if (content->length == 0) {
        FF_DEBUG("PCI IDs data is empty");
        return;
    }

    const char* const fileEnd = content->chars + content->length;
    char needle[8];

    // Search for vendor
    uint32_t len = (uint32_t) snprintf(needle, ARRAY_SIZE(needle), "\n%04x  ", vendor);
    const char* start = memmem(content->chars, content->length, needle, len);
    if (!start) {
        FF_DEBUG("PCI vendor entry 0x%04x was not found", vendor);
        return;
    }

    FF_DEBUG("Found PCI vendor entry for 0x%04x", vendor);
    start += len;
    const char* vendorEnd = memchr(start, '\n', (size_t) (fileEnd - start));
    if (!vendorEnd) {
        vendorEnd = fileEnd;
    }
    if (vendorName && !vendorName->length) {
        ffStrbufSetNS(vendorName, (uint32_t) (vendorEnd - start), start);
    }

    if (!deviceName) {
        return;
    }

    // Device lines are `\tXXXX  name`, sorted and ascending within a vendor block, so one linear
    // walk suffices: stop at the next vendor, or once an id greater than ours is seen.
    char deviceId[8];
    snprintf(deviceId, ARRAY_SIZE(deviceId), "%04x", device);
    for (const char* p = vendorEnd < fileEnd ? vendorEnd + 1 : fileEnd;
         p < fileEnd && (*p == '\t' || *p == '#');) {
        if (*p == '\t' && fileEnd - p >= 7) {
            int order = memcmp(p + 1, deviceId, 4);
            if (order > 0) {
                break;
            }
            if (order == 0 && p[5] == ' ' && p[6] == ' ') {
                FF_DEBUG("Found PCI device entry for 0x%04x:0x%04x", vendor, device);
                const char* dstart = p + 7;
                const char* end = memchr(dstart, '\n', (size_t) (fileEnd - dstart));
                if (!end) {
                    end = fileEnd;
                }

                const char* closingBracket = end - 1;
                if (*closingBracket == ']') {
                    const char* openingBracket = memrchr(dstart, '[', (size_t) (closingBracket - dstart));
                    if (openingBracket) {
                        openingBracket++;
                        if (!deviceName->length) {
                            ffStrbufSetNS(deviceName, (uint32_t) (closingBracket - openingBracket), openingBracket);
                        }
                    }
                }
                if (!deviceName->length) {
                    ffStrbufSetNS(deviceName, (uint32_t) (end - dstart), dstart);
                }
                return;
            }
        }
        p = memchr(p, '\n', (size_t) (fileEnd - p));
        if (!p) {
            return;
        }
        p++;
    }
    FF_DEBUG("PCI device entry 0x%04x:0x%04x was not found in pci.ids", vendor, device);
}

#if FF_HAVE_EMBEDDED_PCIIDS
static inline int pciDeviceCmp(const uint16_t* key, const FFPciDevice* element) {
    return (int) *key - (int) element->id;
}

bool ffPciIdsLookupEmbedded(uint16_t vendor, uint16_t device, FFstrbuf* vendorName, FFstrbuf* deviceName) {
    FF_DEBUG("Searching embedded PCI IDs for vendor=0x%04x device=0x%04x", vendor, device);
    for (const FFPciVendor* pvendor = ffPciVendors; pvendor->name; pvendor++) {
        if (pvendor->id != vendor) {
            continue;
        }

        if (vendorName && !vendorName->length) {
            ffStrbufSetS(vendorName, pvendor->name);
        }

        if (!deviceName) {
            return true;
        }

        const FFPciDevice* pdevice = (const FFPciDevice*) bsearch(&device, pvendor->devices, pvendor->nDevices, sizeof(*pdevice), (void*) pciDeviceCmp);
        if (!pdevice) {
            FF_DEBUG("Embedded PCI vendor matched ('%s'), but device 0x%04x was not found", pvendor->name, device);
            return true;
        }

        uint32_t nameLen = (uint32_t) strlen(pdevice->name);
        if (nameLen == 0) {
            FF_DEBUG("Embedded PCI device entry for 0x%04x:0x%04x has an empty name", vendor, device);
            return true;
        }

        const char* closingBracket = pdevice->name + nameLen - 1;
        if (*closingBracket == ']') {
            const char* openingBracket = memrchr(pdevice->name, '[', nameLen - 1);
            if (openingBracket) {
                openingBracket++;
                if (!deviceName->length) {
                    ffStrbufSetNS(deviceName, (uint32_t) (closingBracket - openingBracket), openingBracket);
                }
            }
        }
        if (!deviceName->length) {
            ffStrbufSetNS(deviceName, nameLen, pdevice->name);
        }
        FF_DEBUG("Embedded PCI ID matched: name='%s'", deviceName->chars);
        return true;
    }
    FF_DEBUG("Embedded PCI vendor 0x%04x was not found", vendor);
    return false;
}
#endif
