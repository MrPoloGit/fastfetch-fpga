#pragma once

#include "common/FFstrbuf.h"

#include <stdint.h>

// Loads the system's `pci.ids` database, caching the result. Empty if none was found - not an
// error, callers should treat that as "no match". Shared by any module resolving PCI names this
// way instead of a hand-written table (`gpu`, `fpga`).
const FFstrbuf* ffPciIdsLoadFile(void);

// Looks up `vendor`/`device` in `content` (from `ffPciIdsLoadFile`, or a custom buffer in tests).
// `vendorName`/`deviceName` may be NULL to skip that half. An already-non-empty buffer is left
// untouched. A `... [Alias]` device name is reduced to just the alias.
void ffPciIdsLookupFile(const FFstrbuf* content, uint16_t vendor, uint16_t device, FFstrbuf* vendorName, FFstrbuf* deviceName);

#if FF_HAVE_EMBEDDED_PCIIDS
// Same lookup against the compiled-in snapshot. Returns whether the vendor was found; if the
// device wasn't, `deviceName` is left empty rather than synthesizing a fallback - that decision is
// left to the caller, since GPU and FPGA fall back differently.
bool ffPciIdsLookupEmbedded(uint16_t vendor, uint16_t device, FFstrbuf* vendorName, FFstrbuf* deviceName);
#endif
