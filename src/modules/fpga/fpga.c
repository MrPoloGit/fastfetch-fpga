#include "common/printing.h"
#include "common/jsonconfig.h"
#include "common/temps.h"
#include "detection/fpga/fpga.h"
#include "modules/fpga/fpga.h"

static void printFPGAResult(FFFPGAOptions* options, uint8_t index, const FFFPGAResult* fpga) {
    if (options->moduleArgs.outputFormat.length == 0) {
        ffPrintLogoAndKey(FF_MODULE_GET_DISPLAY_NAME(FPGA), index, &options->moduleArgs, FF_PRINT_TYPE_DEFAULT);

        FF_STRBUF_AUTO_DESTROY output = ffStrbufCreate();
        ffStrbufAppend(&output, &fpga->name);

        if (fpga->pcieGen != FF_FPGA_PCIE_SPEED_UNSET) {
            ffStrbufAppendF(&output, " @ Gen%u x%u", fpga->pcieGen, fpga->pcieLanes);
        }

        // Sensor/driver detail only shows with `temp: true`, matching `gpu`'s default-line convention.
        if (options->temp) {
            if (fpga->temperature != FF_FPGA_TEMP_UNSET) {
                ffStrbufAppendS(&output, " - ");
                ffTempsAppendNum(fpga->temperature, &output, options->tempConfig, &options->moduleArgs);
            }

            if (fpga->power != FF_FPGA_POWER_UNSET) {
                ffStrbufAppendF(&output, " - %.1fW", fpga->power);
            }

            ffStrbufAppendS(&output, " [");
            ffStrbufAppendS(&output, fpga->driver.length ? fpga->driver.chars : "no driver");
            ffStrbufAppendC(&output, ']');
        }

        ffStrbufPutTo(&output, stdout);
    } else {
        FF_STRBUF_AUTO_DESTROY tempStr = ffStrbufCreate();
        if (fpga->temperature != FF_FPGA_TEMP_UNSET) {
            ffTempsAppendNum(fpga->temperature, &tempStr, options->tempConfig, &options->moduleArgs);
        }

        FF_STRBUF_AUTO_DESTROY powerStr = ffStrbufCreate();
        if (fpga->power != FF_FPGA_POWER_UNSET) {
            ffStrbufAppendF(&powerStr, "%.1fW", fpga->power);
        }

        FF_STRBUF_AUTO_DESTROY pcieStr = ffStrbufCreate();
        if (fpga->pcieGen != FF_FPGA_PCIE_SPEED_UNSET) {
            ffStrbufAppendF(&pcieStr, "Gen%u x%u", fpga->pcieGen, fpga->pcieLanes);
        }

        FF_PRINT_FORMAT_CHECKED(FF_MODULE_GET_DISPLAY_NAME(FPGA), index, &options->moduleArgs, FF_PRINT_TYPE_DEFAULT, ((FFformatarg[]) {
            FF_ARG(fpga->vendor, "vendor"),
            FF_ARG(fpga->name, "name"),
            FF_ARG(fpga->driver, "driver"),
            FF_ARG(fpga->pciAddress, "pci-address"),
            FF_ARG(index, "index"),
            FF_ARG(tempStr, "temperature"),
            FF_ARG(powerStr, "power"),
            FF_ARG(pcieStr, "pcie-speed"),
        }));
    }
}

bool ffPrintFPGA(FFFPGAOptions* options) {
    FF_LIST_AUTO_DESTROY fpgas = ffListCreate();
    const char* error = ffDetectFPGA(options, &fpgas);
    if (error) {
        ffPrintError(FF_MODULE_GET_DISPLAY_NAME(FPGA), 0, &options->moduleArgs, FF_PRINT_TYPE_DEFAULT, "%s", error);
        return false;
    }

    uint32_t i = 0;
    FF_LIST_FOR_EACH (FFFPGAResult, fpga, fpgas) {
        printFPGAResult(options, fpgas.length == 1 ? 0 : (uint8_t) (i + 1), fpga);
        ++i;
    }

    FF_LIST_FOR_EACH (FFFPGAResult, fpga, fpgas) {
        ffStrbufDestroy(&fpga->vendor);
        ffStrbufDestroy(&fpga->name);
        ffStrbufDestroy(&fpga->driver);
        ffStrbufDestroy(&fpga->pciAddress);
    }

    if (fpgas.length == 0) {
        ffPrintError(FF_MODULE_GET_DISPLAY_NAME(FPGA), 0, &options->moduleArgs, FF_PRINT_TYPE_DEFAULT, "No FPGA accelerator cards detected");
        return false;
    }

    return true;
}

void ffParseFPGAJsonObject(FFFPGAOptions* options, yyjson_val* module) {
    yyjson_val *key, *val;
    size_t idx, max;
    yyjson_obj_foreach (module, idx, max, key, val) {
        if (ffJsonConfigParseModuleArgs(key, val, &options->moduleArgs)) {
            continue;
        }

        if (ffTempsParseJsonObject(key, val, &options->temp, &options->tempConfig)) {
            continue;
        }

        ffPrintError(FF_MODULE_GET_DISPLAY_NAME(FPGA), 0, &options->moduleArgs, FF_PRINT_TYPE_DEFAULT, "Unknown JSON key %s", unsafe_yyjson_get_str(key));
    }
}

[[gnu::cold]]
void ffGenerateFPGAJsonConfig(FFFPGAOptions* options, yyjson_mut_doc* doc, yyjson_mut_val* module) {
    ffJsonConfigGenerateModuleArgsConfig(doc, module, &options->moduleArgs);
    ffTempsGenerateJsonConfig(doc, module, options->temp, options->tempConfig);
}

bool ffGenerateFPGAJsonResult(FFFPGAOptions* options, yyjson_mut_doc* doc, yyjson_mut_val* module) {
    FF_LIST_AUTO_DESTROY fpgas = ffListCreate();
    const char* error = ffDetectFPGA(options, &fpgas);
    if (error) {
        yyjson_mut_obj_add_str(doc, module, "error", error);
        return false;
    }

    yyjson_mut_val* arr = yyjson_mut_obj_add_arr(doc, module, "result");
    FF_LIST_FOR_EACH (FFFPGAResult, fpga, fpgas) {
        yyjson_mut_val* obj = yyjson_mut_arr_add_obj(doc, arr);

        yyjson_mut_obj_add_strbuf(doc, obj, "vendor", &fpga->vendor);
        yyjson_mut_obj_add_strbuf(doc, obj, "name", &fpga->name);
        yyjson_mut_obj_add_strbuf(doc, obj, "pciAddress", &fpga->pciAddress);

        if (fpga->driver.length) {
            yyjson_mut_obj_add_strbuf(doc, obj, "driver", &fpga->driver);
        } else {
            yyjson_mut_obj_add_null(doc, obj, "driver");
        }

        yyjson_mut_obj_add_uint(doc, obj, "vendorId", fpga->vendorId);
        yyjson_mut_obj_add_uint(doc, obj, "deviceId", fpga->deviceId);

        if (fpga->temperature != FF_FPGA_TEMP_UNSET) {
            yyjson_mut_obj_add_real(doc, obj, "temperature", fpga->temperature);
        } else {
            yyjson_mut_obj_add_null(doc, obj, "temperature");
        }

        if (fpga->power != FF_FPGA_POWER_UNSET) {
            yyjson_mut_obj_add_real(doc, obj, "power", fpga->power);
        } else {
            yyjson_mut_obj_add_null(doc, obj, "power");
        }

        if (fpga->pcieGen != FF_FPGA_PCIE_SPEED_UNSET) {
            yyjson_mut_val* pcieSpeed = yyjson_mut_obj_add_obj(doc, obj, "pcieSpeed");
            yyjson_mut_obj_add_uint(doc, pcieSpeed, "gen", fpga->pcieGen);
            yyjson_mut_obj_add_uint(doc, pcieSpeed, "lanes", fpga->pcieLanes);
        } else {
            yyjson_mut_obj_add_null(doc, obj, "pcieSpeed");
        }

        ffStrbufDestroy(&fpga->vendor);
        ffStrbufDestroy(&fpga->name);
        ffStrbufDestroy(&fpga->driver);
        ffStrbufDestroy(&fpga->pciAddress);
    }

    return true;
}

void ffInitFPGAOptions(FFFPGAOptions* options) {
    ffOptionInitModuleArg(&options->moduleArgs, "");

    // Default off, same as `gpu`'s `temp` - opt-in only.
    options->temp = false;
    options->tempConfig = (FFColorRangeConfig){ 60, 80 };
}

void ffDestroyFPGAOptions(FFFPGAOptions* options) {
    ffOptionDestroyModuleArg(&options->moduleArgs);
}

FFModuleBaseInfo ffFPGAModuleInfo = {
    .name = "FPGA",
    .description = "Print FPGA accelerator card names, PCI addresses, drivers, temperature, power, etc",
    .displayName = {
        .en = "FPGA",
        .ar = "FPGA",
        .cs = "FPGA",
        .de = "FPGA",
        .es = "FPGA",
        .fi = "FPGA",
        .fr = "FPGA",
        .gl = "FPGA",
        .he = "FPGA",
        .id = "FPGA",
        .it = "FPGA",
        .ja = "FPGA",
        .ko = "FPGA",
        .pl = "FPGA",
        .pt = "FPGA",
        .ru = "FPGA",
        .tr = "FPGA",
        .uk = "FPGA",
        .vi = "FPGA",
        .zh_CN = "FPGA",
        .zh_TW = "FPGA",
    },
    .initOptions = (void*) ffInitFPGAOptions,
    .destroyOptions = (void*) ffDestroyFPGAOptions,
    .parseJsonObject = (void*) ffParseFPGAJsonObject,
    .printModule = (void*) ffPrintFPGA,
    .generateJsonResult = (void*) ffGenerateFPGAJsonResult,
    .generateJsonConfig = (void*) ffGenerateFPGAJsonConfig,
    .formatArgs = FF_FORMAT_ARG_LIST(((FFModuleFormatArg[]){
        { "FPGA vendor", "vendor" },
        { "FPGA model name", "name" },
        { "FPGA driver(s) in use", "driver" },
        { "FPGA PCI address", "pci-address" },
        { "FPGA index", "index" },
        { "FPGA die temperature", "temperature" },
        { "FPGA total board power", "power" },
        { "FPGA max PCIe link speed (gen x lanes)", "pcie-speed" },
    })),
    .defaultOrder = 74,
};
