static bool write_probe_report(const char* path, char* err, size_t errSize) {
    char* json = (char*)VirtualAlloc(nullptr, 524288, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!json) {
        set_message(err, errSize, "Out of memory generating probe report");
        return false;
    }

    size_t used = 0;
    auto append = [&](const char* fmt, ...) -> bool {
        if (used >= 524288) return false;
        va_list ap;
        va_start(ap, fmt);
        int written = _vsnprintf_s(json + used, 524288 - used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (written < 0) {
            used = 524287;
            json[524287] = 0;
            return false;
        }
        used += (size_t)written;
        return true;
    };

    auto append_json_string = [&](const char* text) {
        append("\"");
        for (const unsigned char* p = (const unsigned char*)(text ? text : ""); *p; ++p) {
            switch (*p) {
                case '\\': append("\\\\"); break;
                case '"': append("\\\""); break;
                case '\n': append("\\n"); break;
                case '\r': append("\\r"); break;
                case '\t': append("\\t"); break;
                default:
                    if (*p < 32) append("\\u%04x", *p);
                    else append("%c", *p);
                    break;
            }
        }
        append("\"");
    };

    auto append_hex_bytes = [&](const unsigned char* bytes, unsigned int count) {
        for (unsigned int i = 0; i < count; i++) {
            append("%02X", bytes[i]);
            if (i + 1 < count) append(" ");
        }
    };

    enum ProbeKind {
        PROBE_KIND_GENERIC = 0,
        PROBE_KIND_INFO = 1,
        PROBE_KIND_STATUS = 2,
        PROBE_KIND_CONTROL = 3,
    };

    struct ProbeCallResult {
        bool found;
        bool callable;
        int ret;
        unsigned int size;
        unsigned char buf[0x4000];
        char errorText[64];
    };

    const VfBackendSpec* selected = probe_backend_for_current_gpu();
    unsigned char ffMask[32] = {};
    memset(ffMask, 0xFF, sizeof(ffMask));

    auto run_probe_call = [&](unsigned int id,
                              unsigned int size,
                              unsigned int version,
                              ProbeKind kind,
                              const VfBackendSpec* layout,
                              const unsigned char* maskSeed,
                              size_t maskSeedLen,
                              bool hasNumClocksSeed,
                              unsigned int numClocksSeed) -> ProbeCallResult {
        ProbeCallResult result = {};
        result.ret = -9999;
        result.size = size;
        result.errorText[0] = 0;

        auto func = (NvApiFunc)nvapi_qi(id);
        if (!func) return result;
        result.found = true;

        if (size > sizeof(result.buf)) return result;
        result.callable = true;

        const unsigned int header = (version << 16) | size;
        memcpy(&result.buf[0], &header, sizeof(header));

        if (layout && maskSeed && maskSeedLen > 0) {
            unsigned int maskOffset = 0;
            if (kind == PROBE_KIND_INFO) maskOffset = layout->infoMaskOffset;
            else if (kind == PROBE_KIND_STATUS) maskOffset = layout->statusMaskOffset;
            else if (kind == PROBE_KIND_CONTROL) maskOffset = layout->controlMaskOffset;
            if (maskOffset + maskSeedLen <= size) {
                memcpy(&result.buf[maskOffset], maskSeed, maskSeedLen);
            }
        }

        if (layout && kind == PROBE_KIND_STATUS && hasNumClocksSeed) {
            if (layout->statusNumClocksOffset + sizeof(numClocksSeed) <= size) {
                memcpy(&result.buf[layout->statusNumClocksOffset], &numClocksSeed, sizeof(numClocksSeed));
            }
        }

        result.ret = func(g_app.gpuHandle, result.buf);
        if (result.ret != 0) {
            nvapi_get_error_message(result.ret, result.errorText, sizeof(result.errorText));
        }
        return result;
    };

    ProbeCallResult seedInfo = run_probe_call(
        selected->getInfoId,
        selected->infoBufferSize,
        selected->infoVersion,
        PROBE_KIND_INFO,
        selected,
        ffMask,
        sizeof(ffMask),
        false,
        0);

    unsigned char cachedMask[32] = {};
    unsigned int cachedNumClocks = selected->defaultNumClocks;
    bool cachedSeedAvailable = false;
    if (seedInfo.ret == 0 &&
        selected->infoMaskOffset + sizeof(cachedMask) <= seedInfo.size &&
        selected->infoNumClocksOffset + sizeof(cachedNumClocks) <= seedInfo.size) {
        memcpy(cachedMask, &seedInfo.buf[selected->infoMaskOffset], sizeof(cachedMask));
        memcpy(&cachedNumClocks, &seedInfo.buf[selected->infoNumClocksOffset], sizeof(cachedNumClocks));
        if (cachedNumClocks == 0) cachedNumClocks = selected->defaultNumClocks;
        cachedSeedAvailable = true;
    }

    auto append_probe_result = [&](const char* label,
                                   unsigned int id,
                                   unsigned int size,
                                   unsigned int version,
                                   ProbeKind kind,
                                   const VfBackendSpec* layout,
                                   const char* seedSource,
                                   const unsigned char* maskSeed,
                                   size_t maskSeedLen,
                                   bool hasNumClocksSeed,
                                   unsigned int numClocksSeed,
                                   bool includeFullBytes) {
        ProbeCallResult result = run_probe_call(id, size, version, kind, layout, maskSeed, maskSeedLen, hasNumClocksSeed, numClocksSeed);

        append("      {\n");
        append("        \"label\": ");
        append_json_string(label);
        append(",\n        \"id\": \"0x%08X\",\n", id);
        append("        \"size\": %u,\n", size);
        append("        \"version\": %u,\n", version);
        if (seedSource && *seedSource) {
            append("        \"seed_source\": ");
            append_json_string(seedSource);
            append(",\n");
        }
        if (maskSeed && maskSeedLen > 0) {
            append("        \"seed_mask_hex\": \"");
            append_hex_bytes(maskSeed, (unsigned int)maskSeedLen);
            append("\",");
            append("\n");
        }
        if (hasNumClocksSeed) {
            append("        \"seed_num_clocks\": %u,\n", numClocksSeed);
        }

        if (!result.found) {
            append("        \"found\": false\n");
            append("      }");
            return;
        }

        append("        \"found\": true,\n");
        if (!result.callable) {
            append("        \"callable\": false,\n");
            append("        \"error\": \"buffer too large for built-in probe\"\n");
            append("      }");
            return;
        }

        append("        \"callable\": true,\n");
        append("        \"result\": %d,\n", result.ret);
        append("        \"result_hex\": \"0x%08X\",\n", (unsigned int)result.ret);
        append("        \"result_text\": ");
        append_json_string(result.errorText[0] ? result.errorText : (result.ret == 0 ? "NVAPI_OK" : ""));
        append(",\n");
        append("        \"first_bytes\": \"");
        unsigned int dumpCount = result.size < 64 ? result.size : 64;
        append_hex_bytes(result.buf, dumpCount);
        append("\"");

        if (result.ret == 0 && includeFullBytes) {
            append(",\n        \"full_bytes\": \"");
            append_hex_bytes(result.buf, result.size);
            append("\"");
        }

        if (result.ret == 0 && layout) {
            if (kind == PROBE_KIND_INFO) {
                if (layout->infoMaskOffset + 32 <= result.size && layout->infoNumClocksOffset + 4 <= result.size) {
                    unsigned int assumedNumClocks = 0;
                    memcpy(&assumedNumClocks, &result.buf[layout->infoNumClocksOffset], sizeof(assumedNumClocks));
                    append(",\n        \"assumed_info\": {\n");
                    append("          \"mask_hex\": \"");
                    append_hex_bytes(&result.buf[layout->infoMaskOffset], 32);
                    append("\",\n");
                    append("          \"num_clocks\": %u\n", assumedNumClocks);
                    append("        }");
                }
            } else if (kind == PROBE_KIND_STATUS) {
                int populated = 0;
                unsigned int firstFreq = 0;
                unsigned int firstVolt = 0;
                unsigned int maxFreq = 0;
                if (layout->statusEntriesOffset + 8 <= result.size) {
                    for (int i = 0; i < VF_NUM_POINTS; i++) {
                        unsigned int entryOffset = layout->statusEntriesOffset + (unsigned int)i * layout->statusEntryStride;
                        if (entryOffset + 8 > result.size) break;
                        unsigned int freq = 0;
                        unsigned int volt = 0;
                        memcpy(&freq, &result.buf[entryOffset], sizeof(freq));
                        memcpy(&volt, &result.buf[entryOffset + 4], sizeof(volt));
                        if (freq > 0) {
                            populated++;
                            if (firstFreq == 0) {
                                firstFreq = freq;
                                firstVolt = volt;
                            }
                            if (freq > maxFreq) maxFreq = freq;
                        }
                    }
                    append(",\n        \"assumed_status_parse\": {\n");
                    append("          \"populated_points\": %d,\n", populated);
                    append("          \"first_freq_khz\": %u,\n", firstFreq);
                    append("          \"first_volt_uv\": %u,\n", firstVolt);
                    append("          \"max_freq_khz\": %u\n", maxFreq);
                    append("        }");
                }
            }
        }

        append("\n      }");
    };

    char nvapiVersion[64] = {};
    nvapi_get_interface_version_string(nvapiVersion, sizeof(nvapiVersion));
    SYSTEMTIME now = {};
    GetLocalTime(&now);

    struct PublicCallSummary {
        bool found;
        bool callable;
        int ret;
        char errorText[64];
    };
    auto run_public_summary = [&](unsigned int id, unsigned int size, unsigned int version) -> PublicCallSummary {
        PublicCallSummary summary = {};
        ProbeCallResult result = run_probe_call(id, size, version, PROBE_KIND_GENERIC, nullptr, nullptr, 0, false, 0);
        summary.found = result.found;
        summary.callable = result.callable;
        summary.ret = result.ret;
        StringCchCopyA(summary.errorText, ARRAY_COUNT(summary.errorText), result.errorText);
        return summary;
    };

    const unsigned int psSizes[] = {0x0008, 0x0018, 0x0048, 0x00B0, 0x01C8, 0x0410,
                                     0x0840, 0x1098, 0x1C94, 0x2420, 0x3000};
    int psV2Results[ARRAY_COUNT(psSizes)] = {};
    int psV3Results[ARRAY_COUNT(psSizes)] = {};
    for (size_t i = 0; i < ARRAY_COUNT(psSizes); i++) {
        PublicCallSummary v2 = run_public_summary(0x6FF81213u, psSizes[i], 2);
        PublicCallSummary v3 = run_public_summary(0x6FF81213u, psSizes[i], 3);
        psV2Results[i] = v2.found && v2.callable ? v2.ret : -9999;
        psV3Results[i] = v3.found && v3.callable ? v3.ret : -9999;
    }

    int psOffsetsV2[13] = {};
    bool psOffsetsV2Valid = false;
    ProbeCallResult psScanV2 = run_probe_call(0x6FF81213u, 0x1CF8, 2, PROBE_KIND_GENERIC, nullptr, nullptr, 0, false, 0);
    if (psScanV2.found && psScanV2.callable && psScanV2.ret == 0) {
        psOffsetsV2Valid = true;
        for (int i = 0; i < 13; i++) {
            memcpy(&psOffsetsV2[i], &psScanV2.buf[0x30 + i * 4], sizeof(psOffsetsV2[i]));
        }
    }

    int psOffsetsV3[13] = {};
    bool psOffsetsV3Valid = false;
    ProbeCallResult psScanV3 = run_probe_call(0x6FF81213u, 0x1CF8, 3, PROBE_KIND_GENERIC, nullptr, nullptr, 0, false, 0);
    if (psScanV3.found && psScanV3.callable && psScanV3.ret == 0) {
        psOffsetsV3Valid = true;
        for (int i = 0; i < 13; i++) {
            memcpy(&psOffsetsV3[i], &psScanV3.buf[0x30 + i * 4], sizeof(psOffsetsV3[i]));
        }
    }

    PublicCallSummary powerPoliciesGetInfo = run_public_summary(0x34206D86u, 0x28, 1);
    PublicCallSummary powerPoliciesGetStatus = run_public_summary(0x355C8B8Cu, 0x50, 1);
    bool powerPoliciesSetStatusFound = nvapi_qi(0xAD95F5EDu) != nullptr;
    bool pstates20SetFound = nvapi_qi(0x0F4DAE6Bu) != nullptr;
    bool pstatesInfoExFound = nvapi_qi(0x6048B02Fu) != nullptr;
    bool selectedSetControlFound = nvapi_qi(selected->setControlId) != nullptr;
    bool nvmlReady = nvml_ensure_ready();

    int liveGpuOffsetMHz = g_app.gpuClockOffsetkHz / 1000;
    int liveMemOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    int livePowerLimitPct = g_app.powerLimitPct;
    bool liveFanSupported = g_app.fanSupported;
    int rawGpuOffsetMinMHz = g_app.gpuClockOffsetMinMHz;
    int rawGpuOffsetMaxMHz = g_app.gpuClockOffsetMaxMHz;
    int rawMemOffsetMinMHz = g_app.memClockOffsetMinMHz;
    int rawMemOffsetMaxMHz = g_app.memClockOffsetMaxMHz;
    int rawOffsetReadPstate = g_app.offsetReadPstate;
    int detectedGpuOffsetMHz = g_app.gpuClockOffsetkHz / 1000;
    int detectedMemOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
    int detectedPowerLimitPct = g_app.powerLimitPct;
    bool detectedFanSupported = g_app.fanSupported;

    char offsetDetail[128] = {};
    if (nvml_read_clock_offsets(offsetDetail, sizeof(offsetDetail))) {
        liveGpuOffsetMHz = g_app.gpuClockOffsetkHz / 1000;
        liveMemOffsetMHz = mem_display_mhz_from_driver_khz(g_app.memClockOffsetkHz);
        rawGpuOffsetMinMHz = g_app.gpuClockOffsetMinMHz;
        rawGpuOffsetMaxMHz = g_app.gpuClockOffsetMaxMHz;
        rawMemOffsetMinMHz = g_app.memClockOffsetMinMHz;
        rawMemOffsetMaxMHz = g_app.memClockOffsetMaxMHz;
        rawOffsetReadPstate = g_app.offsetReadPstate;
    }

    if (nvml_read_power_limit()) {
        livePowerLimitPct = g_app.powerLimitPct;
    }

    char fanDetail[128] = {};
    if (nvml_read_fans(fanDetail, sizeof(fanDetail))) {
        liveFanSupported = g_app.fanSupported;
    }

    append("{\n");
    append("  \"tool\": "); append_json_string(APP_NAME); append(",\n");
    append("  \"version\": "); append_json_string(APP_VERSION); append(",\n");
    append("  \"generated_at\": \"%04u-%02u-%02uT%02u:%02u:%02u\",\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    append("  \"gpu\": {\n");
    append("    \"name\": "); append_json_string(g_app.gpuName); append(",\n");
    append("    \"family\": "); append_json_string(gpu_family_name(g_app.gpuFamily)); append(",\n");
    append("    \"arch_info_valid\": %s,\n", g_app.gpuArchInfoValid ? "true" : "false");
    append("    \"pci_info_valid\": %s,\n", g_app.gpuPciInfoValid ? "true" : "false");
    append("    \"architecture\": \"0x%08X\",\n", g_app.gpuArchitecture);
    append("    \"implementation\": \"0x%08X\",\n", g_app.gpuImplementation);
    append("    \"chip_revision\": \"0x%08X\",\n", g_app.gpuChipRevision);
    append("    \"device_id\": \"0x%08X\",\n", g_app.gpuDeviceId);
    append("    \"subsystem_id\": \"0x%08X\",\n", g_app.gpuSubSystemId);
    append("    \"pci_revision_id\": \"0x%08X\",\n", g_app.gpuPciRevisionId);
    append("    \"ext_device_id\": \"0x%08X\"\n", g_app.gpuExtDeviceId);
    append("  },\n");
    append("  \"selected_backend\": {\n");
    append("    \"name\": "); append_json_string(g_app.vfBackend ? g_app.vfBackend->name : "none"); append(",\n");
    append("    \"supported\": %s,\n", (g_app.vfBackend && g_app.vfBackend->supported) ? "true" : "false");
    append("    \"read_supported\": %s,\n", (g_app.vfBackend && g_app.vfBackend->readSupported) ? "true" : "false");
    append("    \"write_supported\": %s,\n", (g_app.vfBackend && g_app.vfBackend->writeSupported) ? "true" : "false");
    append("    \"best_guess_only\": %s,\n", (g_app.vfBackend && g_app.vfBackend->bestGuessOnly) ? "true" : "false");
    append("    \"probe_layout_fallback\": %s,\n", g_app.vfBackend ? "false" : "true");
    append("    \"get_status_id\": \"0x%08X\",\n", selected->getStatusId);
    append("    \"get_info_id\": \"0x%08X\",\n", selected->getInfoId);
    append("    \"get_control_id\": \"0x%08X\",\n", selected->getControlId);
    append("    \"set_control_id\": \"0x%08X\",\n", selected->setControlId);
    append("    \"status_buffer_size\": %u,\n", selected->statusBufferSize);
    append("    \"info_buffer_size\": %u,\n", selected->infoBufferSize);
    append("    \"control_buffer_size\": %u\n", selected->controlBufferSize);
    append("  },\n");
    append("  \"backend_layout_assumptions\": {\n");
    append("    \"family\": "); append_json_string(gpu_family_name(selected->family)); append(",\n");
    append("    \"supported\": %s,\n", selected->supported ? "true" : "false");
    append("    \"read_supported\": %s,\n", selected->readSupported ? "true" : "false");
    append("    \"write_supported\": %s,\n", selected->writeSupported ? "true" : "false");
    append("    \"best_guess_only\": %s,\n", selected->bestGuessOnly ? "true" : "false");
    append("    \"get_info\": {\n");
    append("      \"id\": \"0x%08X\",\n", selected->getInfoId);
    append("      \"buffer_size\": %u,\n", selected->infoBufferSize);
    append("      \"version\": %u,\n", selected->infoVersion);
    append("      \"mask_offset\": %u,\n", selected->infoMaskOffset);
    append("      \"num_clocks_offset\": %u\n", selected->infoNumClocksOffset);
    append("    },\n");
    append("    \"get_status\": {\n");
    append("      \"id\": \"0x%08X\",\n", selected->getStatusId);
    append("      \"buffer_size\": %u,\n", selected->statusBufferSize);
    append("      \"version\": %u,\n", selected->statusVersion);
    append("      \"mask_offset\": %u,\n", selected->statusMaskOffset);
    append("      \"num_clocks_offset\": %u,\n", selected->statusNumClocksOffset);
    append("      \"entries_offset\": %u,\n", selected->statusEntriesOffset);
    append("      \"entry_stride\": %u\n", selected->statusEntryStride);
    append("    },\n");
    append("    \"get_control\": {\n");
    append("      \"id\": \"0x%08X\",\n", selected->getControlId);
    append("      \"buffer_size\": %u,\n", selected->controlBufferSize);
    append("      \"version\": %u,\n", selected->controlVersion);
    append("      \"mask_offset\": %u,\n", selected->controlMaskOffset);
    append("      \"entry_base_offset\": %u,\n", selected->controlEntryBaseOffset);
    append("      \"entry_stride\": %u,\n", selected->controlEntryStride);
    append("      \"entry_delta_offset\": %u\n", selected->controlEntryDeltaOffset);
    append("    },\n");
    append("    \"set_control\": {\n");
    append("      \"id\": \"0x%08X\"\n", selected->setControlId);
    append("    },\n");
    append("    \"default_num_clocks\": %u\n", selected->defaultNumClocks);
    append("  },\n");
    append("  \"nvapi\": {\n");
    append("    \"version_string\": "); append_json_string(nvapiVersion); append("\n");
    append("  },\n");
    append("  \"vf_seed\": {\n");
    append("    \"selected_info_probe_ok\": %s,\n", seedInfo.ret == 0 ? "true" : "false");
    append("    \"assumed_mask_available\": %s,\n", cachedSeedAvailable ? "true" : "false");
    append("    \"assumed_num_clocks\": %u,\n", cachedNumClocks);
    append("    \"assumed_mask_hex\": \"");
    append_hex_bytes(cachedSeedAvailable ? cachedMask : ffMask, 32);
    append("\"\n");
    append("  },\n");
    append("  \"live_state\": {\n");
    append("    \"curve_loaded\": %s,\n", g_app.loaded ? "true" : "false");
    append("    \"populated_points\": %d,\n", g_app.numPopulated);
    append("    \"gpu_offset_mhz\": %d,\n", liveGpuOffsetMHz);
    append("    \"mem_offset_mhz\": %d,\n", liveMemOffsetMHz);
    append("    \"gpu_offset_range_min_mhz\": %d,\n", rawGpuOffsetMinMHz);
    append("    \"gpu_offset_range_max_mhz\": %d,\n", rawGpuOffsetMaxMHz);
    append("    \"mem_offset_range_min_mhz\": %d,\n", rawMemOffsetMinMHz);
    append("    \"mem_offset_range_max_mhz\": %d,\n", rawMemOffsetMaxMHz);
    append("    \"offset_read_pstate\": %d,\n", rawOffsetReadPstate);
    append("    \"power_limit_pct\": %d,\n", livePowerLimitPct);
    append("    \"fan_supported\": %s\n", liveFanSupported ? "true" : "false");
    append("  },\n");
    append("  \"detected_state\": {\n");
    append("    \"gpu_offset_mhz\": %d,\n", detectedGpuOffsetMHz);
    append("    \"mem_offset_mhz\": %d,\n", detectedMemOffsetMHz);
    append("    \"power_limit_pct\": %d,\n", detectedPowerLimitPct);
    append("    \"fan_supported\": %s\n", detectedFanSupported ? "true" : "false");
    append("  },\n");
    append("  \"nvml_capabilities\": {\n");
    append("    \"ready\": %s,\n", nvmlReady ? "true" : "false");
    append("    \"get_power_limit\": %s,\n", g_nvml_api.getPowerLimit ? "true" : "false");
    append("    \"get_power_default_limit\": %s,\n", g_nvml_api.getPowerDefaultLimit ? "true" : "false");
    append("    \"get_power_constraints\": %s,\n", g_nvml_api.getPowerConstraints ? "true" : "false");
    append("    \"set_power_limit\": %s,\n", g_nvml_api.setPowerLimit ? "true" : "false");
    append("    \"get_clock_offsets\": %s,\n", g_nvml_api.getClockOffsets ? "true" : "false");
    append("    \"set_clock_offsets\": %s,\n", g_nvml_api.setClockOffsets ? "true" : "false");
    append("    \"get_perf_state\": %s,\n", g_nvml_api.getPerformanceState ? "true" : "false");
    append("    \"get_gpc_clk_vf_offset\": %s,\n", g_nvml_api.getGpcClkVfOffset ? "true" : "false");
    append("    \"get_mem_clk_vf_offset\": %s,\n", g_nvml_api.getMemClkVfOffset ? "true" : "false");
    append("    \"get_gpc_clk_minmax_vf_offset\": %s,\n", g_nvml_api.getGpcClkMinMaxVfOffset ? "true" : "false");
    append("    \"get_mem_clk_minmax_vf_offset\": %s,\n", g_nvml_api.getMemClkMinMaxVfOffset ? "true" : "false");
    append("    \"set_gpc_clk_vf_offset\": %s,\n", g_nvml_api.setGpcClkVfOffset ? "true" : "false");
    append("    \"set_mem_clk_vf_offset\": %s,\n", g_nvml_api.setMemClkVfOffset ? "true" : "false");
    append("    \"get_num_fans\": %s,\n", g_nvml_api.getNumFans ? "true" : "false");
    append("    \"get_minmax_fan_speed\": %s,\n", g_nvml_api.getMinMaxFanSpeed ? "true" : "false");
    append("    \"get_fan_control_policy\": %s,\n", g_nvml_api.getFanControlPolicy ? "true" : "false");
    append("    \"set_fan_control_policy\": %s,\n", g_nvml_api.setFanControlPolicy ? "true" : "false");
    append("    \"get_fan_speed\": %s,\n", g_nvml_api.getFanSpeed ? "true" : "false");
    append("    \"get_target_fan_speed\": %s,\n", g_nvml_api.getTargetFanSpeed ? "true" : "false");
    append("    \"get_fan_speed_rpm\": %s,\n", g_nvml_api.getFanSpeedRpm ? "true" : "false");
    append("    \"set_fan_speed\": %s,\n", g_nvml_api.setFanSpeed ? "true" : "false");
    append("    \"set_default_fan_speed\": %s,\n", g_nvml_api.setDefaultFanSpeed ? "true" : "false");
    append("    \"get_cooler_info\": %s,\n", g_nvml_api.getCoolerInfo ? "true" : "false");
    append("    \"get_temperature\": %s,\n", g_nvml_api.getTemperature ? "true" : "false");
    append("    \"get_clock\": %s,\n", g_nvml_api.getClock ? "true" : "false");
    append("    \"get_max_clock\": %s\n", g_nvml_api.getMaxClock ? "true" : "false");
    append("  },\n");
    append("  \"public_probe\": {\n");
    append("    \"power_policies_get_info\": {\n");
    append("      \"found\": %s,\n", powerPoliciesGetInfo.found ? "true" : "false");
    append("      \"callable\": %s,\n", powerPoliciesGetInfo.callable ? "true" : "false");
    append("      \"result\": %d,\n", powerPoliciesGetInfo.ret);
    append("      \"result_text\": "); append_json_string(powerPoliciesGetInfo.errorText[0] ? powerPoliciesGetInfo.errorText : (powerPoliciesGetInfo.ret == 0 ? "NVAPI_OK" : "")); append("\n");
    append("    },\n");
    append("    \"power_policies_get_status\": {\n");
    append("      \"found\": %s,\n", powerPoliciesGetStatus.found ? "true" : "false");
    append("      \"callable\": %s,\n", powerPoliciesGetStatus.callable ? "true" : "false");
    append("      \"result\": %d,\n", powerPoliciesGetStatus.ret);
    append("      \"result_text\": "); append_json_string(powerPoliciesGetStatus.errorText[0] ? powerPoliciesGetStatus.errorText : (powerPoliciesGetStatus.ret == 0 ? "NVAPI_OK" : "")); append("\n");
    append("    },\n");
    append("    \"power_policies_set_status_found\": %s,\n", powerPoliciesSetStatusFound ? "true" : "false");
    append("    \"pstates20_set_found\": %s,\n", pstates20SetFound ? "true" : "false");
    append("    \"pstates_info_ex_found\": %s,\n", pstatesInfoExFound ? "true" : "false");
    append("    \"selected_set_control_found\": %s,\n", selectedSetControlFound ? "true" : "false");
    append("    \"pstates20_sizes\": [\n");
    for (size_t i = 0; i < ARRAY_COUNT(psSizes); i++) {
        append("      {\"size\": %u, \"v2_result\": %d, \"v3_result\": %d}%s\n",
            psSizes[i], psV2Results[i], psV3Results[i], (i + 1 < ARRAY_COUNT(psSizes)) ? "," : "");
    }
    append("    ],\n");
    append("    \"pstates20_offset_scan_v2\": {\n");
    for (int i = 0; i < 13; i++) {
        unsigned int offset = 0x30u + (unsigned int)i * 4u;
        append("      \"0x%03X\": %d%s\n", offset, psOffsetsV2Valid ? psOffsetsV2[i] : 0, (i < 12) ? "," : "");
    }
    append("    },\n");
    append("    \"pstates20_offset_scan_v2_valid\": %s,\n", psOffsetsV2Valid ? "true" : "false");
    append("    \"pstates20_offset_scan_v3\": {\n");
    for (int i = 0; i < 13; i++) {
        unsigned int offset = 0x30u + (unsigned int)i * 4u;
        append("      \"0x%03X\": %d%s\n", offset, psOffsetsV3Valid ? psOffsetsV3[i] : 0, (i < 12) ? "," : "");
    }
    append("    },\n");
    append("    \"pstates20_offset_scan_v3_valid\": %s\n", psOffsetsV3Valid ? "true" : "false");
    append("  },\n");
    const unsigned char* selectedMaskSeed = cachedSeedAvailable ? cachedMask : ffMask;
    const char* selectedMaskSeedSource = cachedSeedAvailable ? "cached_get_info_mask" : "ff_mask_fallback";
    unsigned int selectedNumClocksSeed = cachedSeedAvailable ? cachedNumClocks : selected->defaultNumClocks;

    append("  \"control_layout_probe\": {\n");
    ProbeCallResult controlSeed = run_probe_call(selected->getControlId, selected->controlBufferSize, selected->controlVersion,
        PROBE_KIND_CONTROL, selected, selectedMaskSeed, sizeof(cachedMask), false, 0);
    if (controlSeed.ret == 0) {
        int controlPreviewCount = 16;
        append("    \"current_assumption\": {\n");
        append("      \"entry_base_offset\": %u,\n", selected->controlEntryBaseOffset);
        append("      \"entry_stride\": %u,\n", selected->controlEntryStride);
        append("      \"entry_delta_offset\": %u,\n", selected->controlEntryDeltaOffset);
        append("      \"first_deltas\": [");
        for (int i = 0; i < controlPreviewCount; i++) {
            unsigned int deltaOffset = selected->controlEntryBaseOffset + (unsigned int)i * selected->controlEntryStride + selected->controlEntryDeltaOffset;
            int delta = 0;
            if (deltaOffset + sizeof(delta) <= controlSeed.size) memcpy(&delta, &controlSeed.buf[deltaOffset], sizeof(delta));
            append("%s%d", i ? ", " : "", delta);
        }
        append("]\n");
        append("    },\n");

        append("    \"candidate_layouts\": [\n");
        bool firstCandidate = true;
        unsigned int candidateStrides[] = { 24, 28, 32, 36, 40 };
        unsigned int candidateBases[] = { 32, 44, 56, 68, 80 };
        unsigned int candidateDeltaOffsets[] = { 8, 12, 16, 20, 24, 28 };
        for (unsigned int stride : candidateStrides) {
            for (unsigned int baseOffset : candidateBases) {
                for (unsigned int deltaOffset : candidateDeltaOffsets) {
                    int deltas[8] = {};
                    bool inRange = true;
                    int minDelta = 0;
                    int maxDelta = 0;
                    for (int i = 0; i < 8; i++) {
                        unsigned int offset = baseOffset + (unsigned int)i * stride + deltaOffset;
                        if (offset + sizeof(int) > controlSeed.size) {
                            inRange = false;
                            break;
                        }
                        memcpy(&deltas[i], &controlSeed.buf[offset], sizeof(int));
                        if (i == 0 || deltas[i] < minDelta) minDelta = deltas[i];
                        if (i == 0 || deltas[i] > maxDelta) maxDelta = deltas[i];
                    }
                    if (!inRange) continue;
                    if (minDelta < -1500000 || maxDelta > 1500000) continue;
                    append(firstCandidate ? "" : ",\n");
                    firstCandidate = false;
                    append("      {\n");
                    append("        \"entry_base_offset\": %u,\n", baseOffset);
                    append("        \"entry_stride\": %u,\n", stride);
                    append("        \"entry_delta_offset\": %u,\n", deltaOffset);
                    append("        \"first_deltas\": [");
                    for (int i = 0; i < 8; i++) append("%s%d", i ? ", " : "", deltas[i]);
                    append("]\n");
                    append("      }");
                }
            }
        }
        append("\n    ]\n");
    } else {
        append("    \"error\": \"selected get_control probe failed\"\n");
    }
    append("  },\n");
    append("  \"vf_probe\": [\n");

    append_probe_result("selected_get_info", selected->getInfoId, selected->infoBufferSize, selected->infoVersion,
        PROBE_KIND_INFO, selected, "ff_mask_seed", ffMask, sizeof(ffMask), false, 0, true);
    append(",\n");
    append_probe_result("selected_get_info_v2", selected->getInfoId, selected->infoBufferSize, 2,
        PROBE_KIND_INFO, selected, "ff_mask_seed", ffMask, sizeof(ffMask), false, 0, false);
    append(",\n");
    append_probe_result("selected_get_info_alt_size_minus_4", selected->getInfoId, selected->infoBufferSize >= 4 ? (selected->infoBufferSize - 4) : selected->infoBufferSize, selected->infoVersion,
        PROBE_KIND_INFO, selected, "ff_mask_seed", ffMask, sizeof(ffMask), false, 0, false);
    append(",\n");
    append_probe_result("selected_get_info_alt_size_plus_4", selected->getInfoId, selected->infoBufferSize + 4, selected->infoVersion,
        PROBE_KIND_INFO, selected, "ff_mask_seed", ffMask, sizeof(ffMask), false, 0, false);
    append(",\n");
    append_probe_result("selected_get_status_ff_seed", selected->getStatusId, selected->statusBufferSize, selected->statusVersion,
        PROBE_KIND_STATUS, selected, "ff_mask_seed", ffMask, sizeof(ffMask), true, selected->defaultNumClocks, false);
    append(",\n");
    append_probe_result("selected_get_status_cached_seed", selected->getStatusId, selected->statusBufferSize, selected->statusVersion,
        PROBE_KIND_STATUS, selected, selectedMaskSeedSource, selectedMaskSeed, sizeof(cachedMask), true, selectedNumClocksSeed, true);
    append(",\n");
    append_probe_result("selected_get_control_ff_seed", selected->getControlId, selected->controlBufferSize, selected->controlVersion,
        PROBE_KIND_CONTROL, selected, "ff_mask_seed", ffMask, sizeof(ffMask), false, 0, false);
    append(",\n");
    append_probe_result("selected_get_control_cached_seed", selected->getControlId, selected->controlBufferSize, selected->controlVersion,
        PROBE_KIND_CONTROL, selected, selectedMaskSeedSource, selectedMaskSeed, sizeof(cachedMask), false, 0, true);
    append(",\n");
    append_probe_result("selected_get_status_cached_seed_v2", selected->getStatusId, selected->statusBufferSize, 2,
        PROBE_KIND_STATUS, selected, selectedMaskSeedSource, selectedMaskSeed, sizeof(cachedMask), true, selectedNumClocksSeed, false);
    append(",\n");
    append_probe_result("selected_get_control_cached_seed_v2", selected->getControlId, selected->controlBufferSize, 2,
        PROBE_KIND_CONTROL, selected, selectedMaskSeedSource, selectedMaskSeed, sizeof(cachedMask), false, 0, false);
    append(",\n");
    append_probe_result("selected_get_status_cached_seed_alt_size_minus_4", selected->getStatusId, selected->statusBufferSize >= 4 ? (selected->statusBufferSize - 4) : selected->statusBufferSize, selected->statusVersion,
        PROBE_KIND_STATUS, selected, selectedMaskSeedSource, selectedMaskSeed, sizeof(cachedMask), true, selectedNumClocksSeed, false);
    append(",\n");
    append_probe_result("selected_get_status_cached_seed_alt_size_plus_4", selected->getStatusId, selected->statusBufferSize + 4, selected->statusVersion,
        PROBE_KIND_STATUS, selected, selectedMaskSeedSource, selectedMaskSeed, sizeof(cachedMask), true, selectedNumClocksSeed, false);
    append(",\n");
    append_probe_result("blackwell_get_status_cached_seed", g_vfBackendBlackwell.getStatusId, g_vfBackendBlackwell.statusBufferSize, g_vfBackendBlackwell.statusVersion,
        PROBE_KIND_STATUS, &g_vfBackendBlackwell, selectedMaskSeedSource, selectedMaskSeed, sizeof(cachedMask), true, selectedNumClocksSeed, false);
    append("\n  ]\n");
    append("}\n");

    bool ok = write_text_file_atomic(path, json, used, err, errSize);
    VirtualFree(json, 0, MEM_RELEASE);
    return ok;
}

static void close_nvml() {
    if (g_app.nvmlReady && g_nvml_api.shutdown) {
        g_nvml_api.shutdown();
    }
    g_app.nvmlReady = false;
    g_app.nvmlDevice = nullptr;
    if (g_nvml) {
        FreeLibrary(g_nvml);
        g_nvml = nullptr;
    }
    memset(&g_nvml_api, 0, sizeof(g_nvml_api));
}

static bool get_window_text_safe(HWND hwnd, char* buf, int bufSize) {
    if (!buf || bufSize < 1) return false;
    buf[0] = 0;
    if (!hwnd) return false;
    GetWindowTextA(hwnd, buf, bufSize);
    buf[bufSize - 1] = 0;
    trim_ascii(buf);
    return true;
}

static void initialize_desired_settings_defaults(DesiredSettings* desired) {
    if (!desired) return;
    memset(desired, 0, sizeof(*desired));
    desired->lockTracksAnchor = true;
    desired->fanAuto = true;
    desired->fanMode = FAN_MODE_AUTO;
    fan_curve_set_default(&desired->fanCurve);
}

static void set_desired_fan_from_legacy_value(DesiredSettings* desired, bool fanAuto, int fanPercent) {
    if (!desired) return;
    desired->hasFan = true;
    desired->fanAuto = fanAuto;
    desired->fanMode = fanAuto ? FAN_MODE_AUTO : FAN_MODE_FIXED;
    desired->fanPercent = fanPercent;
}

static const char* fan_mode_to_config_value(int mode) {
    switch (mode) {
        case FAN_MODE_FIXED: return "fixed";
        case FAN_MODE_CURVE: return "curve";
        default: return "auto";
    }
}

static bool parse_fan_mode_config_value(const char* text, int* mode) {
    if (!text || !*text || !mode) return false;
    if (streqi_ascii(text, "auto") || streqi_ascii(text, "default")) {
        *mode = FAN_MODE_AUTO;
        return true;
    }
    if (streqi_ascii(text, "fixed") || streqi_ascii(text, "manual")) {
        *mode = FAN_MODE_FIXED;
        return true;
    }
    if (streqi_ascii(text, "curve")) {
        *mode = FAN_MODE_CURVE;
        return true;
    }
    return false;
}

static bool load_fan_curve_config_from_section(const char* path, const char* section, FanCurveConfig* curve, char* err, size_t errSize) {
    if (!curve) return false;
    if (!path || !section || !*section) return false;

    if (!config_section_has_keys(path, section)) return true;

    char buf[64] = {};
    GetPrivateProfileStringA(section, "poll_interval_ms", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        if (!parse_int_strict(buf, &curve->pollIntervalMs)) {
            set_message(err, errSize, "Invalid fan curve poll interval in %s", section);
            return false;
        }
    }

    GetPrivateProfileStringA(section, "hysteresis_c", "", buf, sizeof(buf), path);
    trim_ascii(buf);
    if (buf[0]) {
        if (!parse_int_strict(buf, &curve->hysteresisC)) {
            set_message(err, errSize, "Invalid fan curve hysteresis in %s", section);
            return false;
        }
    }

    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        char key[32] = {};

        StringCchPrintfA(key, ARRAY_COUNT(key), "enabled%d", i);
        GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
        trim_ascii(buf);
        if (buf[0]) {
            int value = 0;
            if (!parse_int_strict(buf, &value)) {
                set_message(err, errSize, "Invalid fan curve enabled flag in %s", section);
                return false;
            }
            curve->points[i].enabled = value != 0;
        }

        StringCchPrintfA(key, ARRAY_COUNT(key), "temp%d", i);
        GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
        trim_ascii(buf);
        if (buf[0]) {
            if (!parse_int_strict(buf, &curve->points[i].temperatureC)) {
                set_message(err, errSize, "Invalid fan curve temperature in %s", section);
                return false;
            }
        }

        StringCchPrintfA(key, ARRAY_COUNT(key), "pct%d", i);
        GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
        trim_ascii(buf);
        if (buf[0]) {
            if (!parse_int_strict(buf, &curve->points[i].fanPercent)) {
                set_message(err, errSize, "Invalid fan curve percentage in %s", section);
                return false;
            }
        }
    }

    fan_curve_normalize(curve);
    if (g_app.fanRangeKnown) {
        fan_curve_clamp_percentages(curve, (int)g_app.fanMinPct, (int)g_app.fanMaxPct);
    }
    return fan_curve_validate(curve, err, errSize);
}

static void append_fan_curve_section_text(char* cfg, size_t cfgSize, size_t* used, const char* sectionName, const FanCurveConfig* curve) {
    if (!cfg || !used || !sectionName || !curve) return;

    auto appendf = [&](const char* fmt, ...) {
        if (*used >= cfgSize - 1) return;
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf_s(cfg + *used, cfgSize - *used, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (n > 0) *used += (size_t)n;
    };

    appendf("[%s]\r\n", sectionName);
    appendf("poll_interval_ms=%d\r\n", curve->pollIntervalMs);
    appendf("hysteresis_c=%d\r\n", curve->hysteresisC);
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        appendf("enabled%d=%d\r\n", i, curve->points[i].enabled ? 1 : 0);
        appendf("temp%d=%d\r\n", i, curve->points[i].temperatureC);
        appendf("pct%d=%d\r\n", i, curve->points[i].fanPercent);
    }
    appendf("\r\n");
}

static bool load_desired_settings_from_ini(const char* path, DesiredSettings* desired, char* err, size_t errSize) {
