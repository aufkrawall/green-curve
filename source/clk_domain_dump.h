// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#ifndef GREEN_CURVE_CLK_DOMAIN_DUMP_H
#define GREEN_CURVE_CLK_DOMAIN_DUMP_H

// Include after gpu_backend_xbar.h. This diagnostic deliberately bypasses the
// interpretation gate, never the gate used by normal readers and writers.
#include <stdarg.h>
#include <stdlib.h>

struct ClkDomainDumpReport {
    static constexpr size_t capacity = 512u * 1024u;
    char* text;
    size_t size;
    bool complete;

    ClkDomainDumpReport() : text((char*)calloc(1, capacity)), size(0),
                            complete(text != nullptr) {}
    ~ClkDomainDumpReport() { free(text); }
    ClkDomainDumpReport(const ClkDomainDumpReport&) = delete;
    ClkDomainDumpReport& operator=(const ClkDomainDumpReport&) = delete;

#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    void append(const char* format, ...) {
        if (!complete) return;
        va_list args;
        va_start(args, format);
        int written = vsnprintf(text + size, capacity - size, format, args);
        va_end(args);
        if (written < 0 || (size_t)written >= capacity - size) {
            complete = false;
            return;
        }
        size += (size_t)written;
    }
};

struct ClkDomainDumpApi {
    NvApiFunc getInfo;
    NvApiFunc getControl;
    NvApiFunc getAllClocks;
    NvApiFunc measure;
    int (*getDriverVersion)(unsigned int*, char*);
};

// Resolve only read entry points. In particular, do not reuse the read/write
// API bundles of the normal tuning backend or the actuating domain probe.
static inline ClkDomainDumpApi clk_domain_dump_api(void* (*query)(unsigned int)) {
    ClkDomainDumpApi api = {};
    if (!query) return api;
    api.getInfo = (NvApiFunc)query(0x64B43A6Au);
    api.getControl = (NvApiFunc)query(XBAR_NVAPI_CLK_DOMAINS_GET_CONTROL);
    api.getAllClocks = (NvApiFunc)query(0xDCB616C3u);
    api.measure = (NvApiFunc)query(XBAR_NVAPI_CLK_MEASURE);
    api.getDriverVersion = (int (*)(unsigned int*, char*))query(0x2926AAADu);
    return api;
}

struct ClkDomainDumpIdentity {
    const char* platform;
    const char* version;
    unsigned int build;
    const char* gpuName;
    unsigned int architecture;
    unsigned int deviceId;
    unsigned int subsystemId;
};

static inline void clk_domain_dump_raw(ClkDomainDumpReport* report,
                                       const unsigned char* data, unsigned int size) {
    // Four words per line, including zeros. Offsets are bytes from the start
    // of this response, not inferred domain-entry offsets. The last row may
    // contain fewer than four words (notably the 0x61a4 control size).
    for (unsigned int offset = 0; offset < size; offset += 16) {
        report->append("+%05X", offset);
        for (unsigned int word = offset; word < offset + 16 && word < size; word += 4)
            report->append(" %08X", xbar_get_u32(data, word));
        report->append("\n");
    }
}

static inline const char* clk_domain_dump_layout(const unsigned char* buffer,
                                                 int status) {
    if (status != 0) return "request-refused";
    const XbarClkDomainsSchema* schema =
        xbar_schema_for_version_word(xbar_get_u32(buffer, 0));
    if (!schema) return "unknown-version";
    XbarBufferLayout layout = {};
    return xbar_layout_for_buffer(buffer, schema, &layout) ? "validated" : "mismatch";
}

// Returns true when at least one ClkDomains GET succeeds and the entire report
// fits. A successful unknown layout is useful evidence, not a diagnostic error.
// Missing/refused reads are retained in a partial report, including statuses.
static inline bool clk_domain_dump_collect(ClkDomainDumpReport* report, void* gpu,
                                            const ClkDomainDumpApi& api,
                                            const ClkDomainDumpIdentity& identity) {
    if (!report || !report->complete) return false;
    char name[97] = {};
    if (identity.gpuName) {
        for (size_t i = 0; i + 1 < sizeof(name) && identity.gpuName[i]; ++i) {
            unsigned char c = (unsigned char)identity.gpuName[i];
            name[i] = c >= 32 && c < 127 ? (char)c : '?';
        }
    }
    report->append("# Green Curve clock-domain capture format=1\n"
        "platform=%s version=%s build=%u\n"
        "gpu=%s architecture=0x%08X device=0x%08X subsystem=0x%08X\n"
        "Read-only; no GPU settings or service state are changed.\n"
        "Raw driver units; no VF scaling or domain-name assumptions.\n"
        "All raw words include zeros; rejected requests may contain only request echoes.\n",
        identity.platform ? identity.platform : "unknown",
        identity.version ? identity.version : "unknown", identity.build,
        name[0] ? name : "unavailable", identity.architecture,
        identity.deviceId, identity.subsystemId);
    unsigned int driverVersion = 0;
    char branch[64] = {};
    if (api.getDriverVersion) {
        int status = api.getDriverVersion(&driverVersion, branch);
        report->append("driver: status=%d version_raw=%u\n", status, driverVersion);
    } else report->append("driver: unavailable\n");

    unsigned char* buffer = (unsigned char*)calloc(1, XBAR_CONTROL_BUF_SIZE);
    if (!buffer) {
        report->append("clk_dump: allocation-failed\n");
        return false;
    }
    bool accepted = false;
    unsigned int infoReads = 0, controlReads = 0;
    if (api.getInfo && gpu) {
        static const unsigned int infoVersions[] = {0x00010928u, 0x00020928u, 0x00030928u};
        for (unsigned int request : infoVersions) {
            // A refused request may have changed memory: never carry that
            // response into the next version attempt.
            memset(buffer, 0, XBAR_CONTROL_BUF_SIZE);
            xbar_put_u32(buffer, 0, request);
            int status = api.getInfo(gpu, buffer);
            ++infoReads;
            accepted = accepted || status == 0;
            report->append("\nget-info: request=0x%08X returned=0x%08X status=%d captured=0x0928\n",
                request, xbar_get_u32(buffer, 0), status);
            // GET_INFO's three known request versions share the declared size.
            clk_domain_dump_raw(report, buffer, 0x928u);
        }
    } else report->append("get-info: unavailable (%s)\n", gpu ? "entry point" : "GPU handle");
    if (api.getControl && gpu) {
        for (const XbarClkDomainsSchema& schema : g_xbarSchemas) {
            // Retain the normal mask and an all-domain read attempt: Pascal's
            // domain bit assignments need not fit the modern eight-bit mask.
            const unsigned int masks[] = {schema.requestMask, 0xffffffffu};
            unsigned int count = schema.requestMask == 0xffffffffu ? 1u : 2u;
            for (unsigned int index = 0; index < count; ++index) {
                memset(buffer, 0, XBAR_CONTROL_BUF_SIZE);
                xbar_put_u32(buffer, 0, schema.versionWord);
                xbar_put_u32(buffer, XBAR_CONTROL_MASK_OFFSET, masks[index]);
                int status = api.getControl(gpu, buffer);
                ++controlReads;
                accepted = accepted || status == 0;
                report->append("\nget-control: request=0x%08X mask=0x%08X returned=0x%08X"
                    " returned_mask=0x%08X status=%d layout=%s captured=0x%05X\n",
                    schema.versionWord, masks[index], xbar_get_u32(buffer, 0),
                    xbar_get_u32(buffer, XBAR_CONTROL_MASK_OFFSET), status,
                    clk_domain_dump_layout(buffer, status), (unsigned int)XBAR_CONTROL_BUF_SIZE);
                // Capture the entire zero-initialized capacity even if validation
                // fails or the returned version is unknown. Never follow its size.
                clk_domain_dump_raw(report, buffer, XBAR_CONTROL_BUF_SIZE);
            }
        }
    } else report->append("get-control: unavailable (%s)\n", gpu ? "entry point" : "GPU handle");
    free(buffer);

    struct PublicClocks {
        unsigned int version;
        unsigned int clockType;
        struct { unsigned int present; unsigned int frequency; } domains[32];
    } clocks = {};
    static_assert(sizeof(PublicClocks) == 264, "public clock layout");
    if (api.getAllClocks && gpu) {
        clocks.version = 0x00030000u | sizeof(clocks);
        int status = api.getAllClocks(gpu, &clocks);
        report->append("\npublic-clocks: request=0x00030108 returned=0x%08X status=%d type=current\n",
                       clocks.version, status);
        for (unsigned int id = 0; id < 32; ++id)
            report->append("public-clock: id=%u flags=0x%08X frequency_khz=%u\n",
                id, clocks.domains[id].present, clocks.domains[id].frequency);
    } else report->append("public-clocks: unavailable\n");
    if (api.measure && gpu) {
        for (unsigned int id = 0; id < 32; ++id) {
            unsigned int params[3] = {XBAR_NVAPI_CLK_MEASURE_VERSION, id, 0};
            int status = api.measure(gpu, params);
            report->append("measure: request=0x%08X id=%u returned=0x%08X"
                " returned_id=%u status=%d value_raw=%u\n",
                XBAR_NVAPI_CLK_MEASURE_VERSION, id, params[0], params[1], status, params[2]);
        }
    } else report->append("measure: unavailable\n");
    report->append("\nclk_dump: info_reads=%u control_reads=%u domain_get_accepted=%u complete=yes\n",
                   infoReads, controlReads, accepted ? 1u : 0u);
    return accepted && report->complete;
}

#endif
