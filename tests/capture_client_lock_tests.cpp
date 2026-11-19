// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
// Load-free lock evidence for Import current GPU settings: the driver's
// client lock table decoded from the three locks observed on hardware, the
// capture fields it fills, and the pin/flatten proposals and rows built from
// them (12273-12289).
#include "nvapi_client_lock_table.h"
#include "hardware_capture_getters.h"
#include "profile_capture_proposal_fixture.h"
#include "profile_summary.h"
#include <memory>
#include <string>

namespace {
using profile_capture_proposal_fixture::curve;

struct Table {
    unsigned char bytes[NVAPI_CLIENT_LOCK_TABLE_SIZE]{};
    Table() {
        const unsigned int word = NVAPI_CLIENT_LOCK_TABLE_VERSION_WORD, count = 7;
        memcpy(bytes, &word, 4);
        memcpy(bytes + NVAPI_CLIENT_LOCK_TABLE_COUNT_OFFSET, &count, 4);
        for (unsigned int i = 0; i < count; ++i) put(i, 0, i);
    }
    void put(unsigned int entry, unsigned int field, unsigned int value) {
        memcpy(bytes + NVAPI_CLIENT_LOCK_TABLE_ENTRIES_OFFSET + entry * NVAPI_CLIENT_LOCK_TABLE_ENTRY_STRIDE + field,
               &value, 4);
    }
    void lock(unsigned int entry, unsigned int mode, unsigned int value) {
        put(entry, NVAPI_CLIENT_LOCK_ENTRY_MODE_OFFSET, mode);
        put(entry, NVAPI_CLIENT_LOCK_ENTRY_VALUE_OFFSET, value);
    }
    NvapiClientLocks decode(int status = 0) const { return nvapi_client_locks_decode(bytes, sizeof(bytes), status); }
};
// The tables read on hardware: idle, NVML 1500/1500, NVML 0/1500, and a
// 925 mV curve-point lock held by another tool.
Table idle() { return Table{}; }
Table nvml_pin() {
    Table t; t.lock(0, 2, 1500000); t.lock(1, 2, 1500000); return t;
}
Table nvml_ceiling() {
    Table t; t.lock(0, 2, 1500000); t.lock(1, 2, 0); return t;
}
Table voltage_lock() {
    Table t; t.lock(6, 3, 925000); return t;
}

static Table g_driverTable;
static int g_driverStatus = 0, g_lockReads = 0;
static int get_lock_table(void*, void* buffer) {
    ++g_lockReads;
    unsigned int word = 0;
    memcpy(&word, buffer, 4);
    if (word != NVAPI_CLIENT_LOCK_TABLE_VERSION_WORD) return -9;
    if (!g_driverStatus) memcpy(buffer, g_driverTable.bytes, sizeof(g_driverTable.bytes));
    return g_driverStatus;
}
static void* lock_query(unsigned int id) {
    return id == NVAPI_CLIENT_LOCK_TABLE_GET ? (void*)get_lock_table : nullptr;
}
static void* no_query(unsigned int) { return nullptr; }

// A pushed-tail flatten: anchor 925 mV at 2482 MHz, tail offsets past the range.
HardwareCaptureObservation flattened() {
    HardwareCaptureObservation o = curve();
    o.pointCount = 10;
    for (int i = 0; i < 10; ++i) {
        auto& p = o.points[i];
        p = {};
        p.index = 70 + i;
        p.voltageUv = i < 3 ? 900000 + i * 6250 : 925000 + (i - 3) * 6250;  // anchor at 925 mV
        p.writableState = p.offsetValid = p.frequencyValid = 1;
        p.frequencyHz = i < 3 ? 2392000000LL + i * 30000000LL : 2482000000LL;
        p.offsetHz = i < 3 ? 3199000 - i * 1000000 : i == 3 ? 0 : -2045000000LL;
    }
    return o;
}
void with_locks(HardwareCaptureObservation& o, const NvapiClientLocks& locks) {
    hardware_capture_apply_client_locks(locks, o);
    hardware_capture_domain(o, PROFILE_DOMAIN_LOCK, hardware_capture_client_lock_status(locks), locks.status);
    o.fingerprint = hardware_capture_fingerprint(o);
}
bool prepare(const HardwareCaptureObservation& o, ProfileCaptureProposals& p) {
    char why[256]{};
    return hardware_capture_observation_valid(o) &&
        profile_capture_prepare_proposals(o, {0, true, true}, p, why, sizeof(why));
}
}  // namespace

int run_capture_client_lock_tests() {
    // Decoder: the four hardware tables.
    const auto none = idle().decode();
    if (!none.known || none.graphicsMaxPresent || none.graphicsMinPresent || none.voltagePresent || none.entryCount != 7)
        return 12273;
    const auto pin = nvml_pin().decode();
    if (!pin.known || !pin.graphicsMaxPresent || !pin.graphicsMinPresent || pin.graphicsMaxHz != 1500000000 ||
        pin.graphicsMinHz != 1500000000 || pin.voltagePresent)
        return 12274;
    const auto ceiling = nvml_ceiling().decode();
    if (!ceiling.known || !ceiling.graphicsMaxPresent || ceiling.graphicsMinPresent || ceiling.graphicsMaxHz != 1500000000)
        return 12275;
    const auto volt = voltage_lock().decode();
    if (!volt.known || volt.graphicsMaxPresent || volt.graphicsMinPresent || !volt.voltagePresent || volt.voltageUv != 925000)
        return 12276;

    // Anything not observed is unknown, never guessed.
    Table t = idle(); t.lock(2, 2, 1500000);                       // memory-like entry
    if (t.decode().known || t.decode().rejectedIndex != 2) return 12277;
    t = idle(); t.lock(0, 3, 925000);                              // voltage mode on a clock entry
    if (t.decode().known) return 12277;
    t = idle(); t.lock(6, 2, 1500000);                             // clock mode on the voltage entry
    if (t.decode().known) return 12277;
    t = idle(); t.lock(0, 2, 99);                                  // implausible clock
    if (t.decode().known || t.decode().status != NVAPI_CLIENT_LOCK_REFUSED_VALUE) return 12277;
    t = idle(); t.lock(6, 3, 5000000);                             // implausible voltage
    if (t.decode().known) return 12277;
    t = idle(); t.lock(1, 2, 1500000);                             // floor without ceiling
    if (t.decode().known) return 12277;
    t = idle(); t.lock(0, 2, 1000000); t.lock(1, 2, 1500000);       // floor above ceiling
    if (t.decode().known) return 12277;
    t = idle(); t.put(3, 0, 9);                                    // index mismatch
    if (t.decode().known || t.decode().status != NVAPI_CLIENT_LOCK_REFUSED_ENTRY) return 12278;
    t = idle(); { const unsigned int count = 33; memcpy(t.bytes + NVAPI_CLIENT_LOCK_TABLE_COUNT_OFFSET, &count, 4); }
    if (t.decode().known || t.decode().status != NVAPI_CLIENT_LOCK_REFUSED_HEADER) return 12278;
    t = idle(); t.bytes[2] = 2;                                    // version word
    if (t.decode().known || t.decode().status != NVAPI_CLIENT_LOCK_REFUSED_HEADER) return 12278;
    if (idle().decode(-5).known || idle().decode(-5).status != -5) return 12278;

    // Reader: only the getter is resolved and called, with a v1 request.
    g_driverTable = voltage_lock(); g_driverStatus = 0; g_lockReads = 0;
    int dummy = 0;
    const auto read = nvapi_client_locks_read(lock_query, &dummy);
    if (!read.known || !read.voltagePresent || g_lockReads != 1) return 12279;
    if (nvapi_client_locks_read(no_query, &dummy).status != -100001 ||
        nvapi_client_locks_read(lock_query, nullptr).status != -100001)
        return 12279;
    char line[256]{};
    nvapi_client_locks_describe(read, line, sizeof(line));
    if (std::string(line) != "client-locks: known entries=7 graphicsMin=0 graphicsMax=0 voltageUv=925000") return 12279;

    // Capture fields and wire validation (schema 4).
    auto o = std::make_unique<HardwareCaptureObservation>(flattened());
    with_locks(*o, ceiling);
    if (!hardware_capture_observation_valid(*o) || !o->pinGetterValid || !o->pinPresent || o->pinMinimumHz ||
        o->pinMaximumHz != 1500000000 || o->pinVoltageLocked || !(o->capturedDomains & PROFILE_DOMAIN_LOCK))
        return 12280;
    with_locks(*o, volt);
    if (!hardware_capture_observation_valid(*o) || o->pinPresent || !o->pinVoltageLocked || o->pinVoltageUv != 925000)
        return 12280;
    auto bad = std::make_unique<HardwareCaptureObservation>(*o);
    bad->pinVoltageUv = 0;
    if (hardware_capture_observation_valid(*bad)) return 12281;
    *bad = *o; bad->pinGetterValid = 0;
    if (hardware_capture_observation_valid(*bad)) return 12281;
    *bad = *o; bad->pinVoltageLocked = 0;
    if (hardware_capture_observation_valid(*bad)) return 12281;
    *bad = *o; bad->pinPresent = 1; bad->pinMinimumHz = 0; bad->pinMaximumHz = 0;
    if (hardware_capture_observation_valid(*bad)) return 12281;
    with_locks(*o, idle().decode(-5));
    if (!hardware_capture_observation_valid(*o) || o->pinGetterValid || o->pinVoltageLocked ||
        (o->capturedDomains & PROFILE_DOMAIN_LOCK))
        return 12281;

    // Proposals: a voltage lock on the flattened curve is a pin at that point's
    // frequency, agrees with the flatten and is recommended.
    with_locks(*o, volt);
    auto p = std::make_unique<ProfileCaptureProposals>();
    if (!prepare(*o, *p) || !p->pin.selectable() || p->pin.frequencyHz != 2482000000LL ||
        p->pin.voltageLockUv != 925000 || p->pin.anchorVoltageUv != 925000 ||
        p->pin.provenance != PROFILE_LOCK_PROVENANCE_DRIVER_GETTER || !p->sameTarget ||
        p->suggested != PROFILE_CAPTURE_PIN || p->exact.source.pinKnowledge != PROFILE_PIN_PRESENT)
        return 12282;
    // A fractional-MHz point rounds down: never above the locked point.
    o->points[3].frequencyHz = 2482750000LL;
    for (int i = 4; i < 10; ++i) o->points[i].frequencyHz = 2482750000LL;
    if (!prepare(*o, *p) || p->pin.frequencyHz != 2482000000LL) return 12283;
    for (int i = 3; i < 10; ++i) o->points[i].frequencyHz = 2482000000LL;
    // A voltage that names no curve point is not turned into a guessed clock.
    o->pinVoltageUv = 926000;
    if (!prepare(*o, *p) || p->pin.recognized || p->pin.selectable()) return 12284;
    o->pinVoltageUv = 925000;
    // A voltage lock next to a clock limit needs an explicit decision.
    o->pinPresent = 1; o->pinMaximumHz = 2000000000;
    if (!prepare(*o, *p) || p->pin.recognized || p->suggested != PROFILE_CAPTURE_FLATTEN) return 12285;
    o->pinPresent = 0; o->pinMaximumHz = 0;

    // A ceiling with no floor is reported, never saved as a fixed lock; the
    // flatten stays recommended.
    with_locks(*o, ceiling);
    if (!prepare(*o, *p) || p->pin.recognized || p->suggested != PROFILE_CAPTURE_FLATTEN ||
        std::string(p->pin.reason).find("caps the clock at 1500 MHz") == std::string::npos ||
        p->exact.source.pinKnowledge != PROFILE_PIN_PRESENT || p->exact.lock.hasObservedPinRange)
        return 12286;
    const auto capped = profile_capture_summary(*o, p.get());
    if (capped.curve.text.find("clock capped at 1500 MHz") == std::string::npos) return 12287;

    // Rows and labels name the lock.
    with_locks(*o, volt);
    if (!prepare(*o, *p)) return 12288;
    const auto locked = profile_capture_summary(*o, p.get());
    if (locked.curve.text.find("Flat at 2482 MHz from 925 mV") == std::string::npos ||
        locked.curve.text.find("locked at 925 mV") == std::string::npos ||
        profile_capture_choice_label(*p, PROFILE_CAPTURE_PIN).find("925 mV") == std::string::npos)
        return 12288;
    with_locks(*o, pin);
    if (!prepare(*o, *p) || !p->pin.recognized || p->pin.frequencyHz != 1500000000 || p->sameTarget ||
        !p->conflictingTargets || p->suggested != PROFILE_CAPTURE_EXACT ||
        !p->exact.lock.hasObservedPinRange || p->exact.lock.observedPinMinimumHz != 1500000000)
        return 12289;
    if (profile_capture_choice_label(*p, PROFILE_CAPTURE_PIN).find("read from the driver") == std::string::npos)
        return 12289;
    return 0;
}
