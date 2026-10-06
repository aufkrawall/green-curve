# GPU Backend

## Summary

The GPU backend handles VF-curve read/write, clock offsets, power limit, and fan control through dynamically loaded NVIDIA driver interfaces (NVAPI private + NVML public).

Current clock transition/reset/verification invariants are documented in
[clock-transition-audit.md](clock-transition-audit.md), updated 2026-09-17.
Derived selective points own offsets, explicit points own absolute MHz, and
failed recovery retains independently tracked protection. Earlier incident
narratives below are historical observations, not acceptance of every current
transition/failure path.

## Driver interfaces

### NVAPI (private, dynamically loaded)

- Loaded at runtime from the installed NVIDIA driver via `nvapi64.dll`
- Uses NVAPI entry point IDs (not named exports) to resolve function pointers
- Key IDs defined in `source/gpu_core.h`:
  - `NVAPI_INIT_ID` (`0x0150E828u`)
  - `NVAPI_ENUM_GPU_ID` (`0xE5AC921Fu`)
  - `NVAPI_GET_NAME_ID` (`0xCEEE8E9Fu`)
  - `NVAPI_GPU_GET_PCI_IDENTIFIERS_ID` (`0x2DDFB66Eu`)
  - `NVAPI_GPU_GET_ARCH_INFO_ID` (`0xD8265D24u`)
- VF-curve control uses `VfBackendSpec` structs with per-family buffer layouts, offsets, and sizes

### NVML (public, dynamically loaded)

- Loaded at runtime from `nvml.dll` on Windows or `libnvidia-ml.so.1` on Linux
- Function pointer types and `NvmlApi` are defined in `source/gpu_core.h`
- Struct stored in `g_nvml_api` (`NvmlApi` struct)
- Used for: power limit, clock offsets (NVML API), fan control, temperature, clock reading
- **Memory-offset units (cross-platform parity, 2026-09-06):** NVML memory-clock
  offsets are EFFECTIVE MHz (2x actual) on both OSes; the canonical
  `DesiredSettings.memOffsetMHz` / `mem_offset_mhz` unit is display/actual MHz.
  Windows halves on read (`mem_display_mhz_from_driver_mhz()`) and doubles on
  write; Linux converts at the semantic boundaries (apply/capture/restore/range
  publish) via `nvml_mem_effective_mhz_from_display_mhz()` /
  `nvml_mem_display_mhz_from_effective_mhz()` in `source/gpu_core.h`. The
  universal IPC cap is `+/-3000` MHz display. Grid verification stays in
  effective units (MEM step 2).
- **Stored-unit migration (Linux, 2026-09-07):** the parity fix shipped
  unreleased after 0.25.0, so every Linux `config.ini`, daemon `active.bin`,
  and `startup.bin` written before it stores effective MHz. On load, Linux
  converts these exactly once to display MHz (halve, trunc toward zero) via
  `source/linux_profile_mem_migration.h` (INI, `[meta] linux_mem_migrated`
  marker) and the v1->v2 / v2->v3 record migrations in
  `source/linux_daemon_state.h`. Order matters: halve THEN clamp, so a stored
  +5000 effective becomes +2500 display and survives the +/-3000 IPC cap.

## GPU family detection

`source/gpu_core.h`:

Architecture IDs from `NvGpuArchitectureId` are mapped to `GpuFamily` enum:
- `NV_GPU_ARCHITECTURE_GP100` (0x130) → `GPU_FAMILY_PASCAL`
- `NV_GPU_ARCHITECTURE_TU100` (0x160) → `GPU_FAMILY_TURING`
- `NV_GPU_ARCHITECTURE_GA100` (0x170) → `GPU_FAMILY_AMPERE`
- `NV_GPU_ARCHITECTURE_AD100` (0x190) → `GPU_FAMILY_LOVELACE`
- `NV_GPU_ARCHITECTURE_GB200` (0x1B0) → `GPU_FAMILY_BLACKWELL`

## VF backend dispatch

`VfBackendSpec` (defined in `source/gpu_core.h`, with tables in `source/vf_backends.cpp`) describes per-family:
- NVAPI function IDs for get status, get info, get control, set control
- Buffer sizes, versions, and field offsets for private NVAPI structs
- Whether read/write/best-effort fallback is supported

## Integrated-SoC control surface (F-CAP)

Family detection answers exactly one question: which private NVAPI struct
layout this architecture uses. It has never answered which control *domains* a
given board exposes, because on every validated discrete GeForce board the
answer was "all of them". Integrated Grace/Blackwell parts (NVIDIA RTX Spark /
GB10, and the Tegra/Jetson lineage before them) break that assumption while
still reporting a Blackwell architecture.

`source/gpu_capability_policy.h` adds a pure per-domain capability layer:

- `GpuDomainCapability` per mutation domain: `UNPROBED` / `AVAILABLE` /
  `REFUSED` / `ABSENT`, plus a `GpuMemoryTopology` and a derived
  `GpuControlSurfaceClass` (full / partial / monitor-only).
- **Core invariant:** the layer only ever *subtracts* a domain after the driver
  actually refused it. A zero-initialized `GpuCapabilityProbe` reports every
  domain available and `gpu_capability_is_complete()` true, so it reproduces
  pre-F-CAP behavior exactly on x64 and on every validated discrete family.
  Asserted at compile time in `vf_backends.cpp`
  (`gpu_capability_available_domains(nullptr) == SERVICE_MUTATION_DOMAIN_ALL`)
  and pinned from both directions by regression codes 2100–2166.
- The capability indices are bit positions of `ServiceMutationDomain`; the two
  representations are bound by static_asserts in `vf_backends.cpp` (not in
  `service_protocol.h`, which is included from inside `gpu_core.h` and so
  cannot include a header that includes `gpu_core.h`).
- **Publication invariant:** `gpu_probe_control_surface()` must assign its
  local result into `g_app.gpuCapability` on BOTH exit paths (the NVML-not-
  ready early return included). b544f7d deleted both assignments while
  replacing the ID scanner; because a zeroed probe reports "everything
  available", nothing user-visible broke and the discarded results — real
  REFUSED domains, memory topology, XBAR classification — went unnoticed
  until the 2026-08-23 review. Pinned by
  `security_gates.check_ipc_transport_and_probe_gates()`
  (`require_text_count(..., "g_app.gpuCapability = probe;", 2)`).

`gpu_capability_classify()` is deliberately conservative: an **unresolved entry
point means an older driver, not absent hardware**, and stays `UNPROBED`. Only
positive evidence (the driver answered, and answered "none") downgrades a
domain. Without this, existing x64 installs on drivers predating an optional
API they never use would start raising the new warning.

`source/gpu_capability_probe.cpp` (Windows shard, included by `main_shell.cpp`
after the NVML shard) performs the read-only probe at the end of
`hardware_initialize()`. It uses only already-resolved NVML entry points and
**never writes GPU state**; `build.py` forbids `setClockOffsets`,
`setPowerLimit` and `setFanSpeed` appearing in it. Deliberately *not* inferred:
a `min == max` power window does **not** mark the power domain absent, because
fixed-TGP laptop boards that work today report exactly that.

The POWER domain is classified from **`nvmlDeviceGetPowerManagementLimit`**, not
from the constraints call. Green Curve expresses this domain as a percentage of
the board default, so the constraints window alone is not a control surface: a
board that answers `min=1000 mW max=130000 mW` and refuses the limit itself
(observed 2026-09-07, RTX 3060 Laptop GPU) has a power target Green Curve cannot
drive. Reporting that `available` is what produced an editable-but-inert 0%
field with no explanation. See the power-target section below.

Two independent GUI warning tiers, each with its own persistent opt-out so
silencing one never silences the other:

| Tier | Condition | Config key |
|---|---|---|
| Unrecognized family | `bestGuessOnly` backend | `[warnings] hide_unrecognized_gpu_warning` |
| Reduced control surface | recognized family **and** (incomplete probe **or** unified memory) | `[warnings] hide_limited_control_warning` |

The second tier has **two** independent triggers, both positive-evidence-only:

1. A domain the driver actually refused.
2. **A recognized *discrete* family reporting a UNIFIED memory pool.** That
   pairing is a contradiction — every validated GeForce card owns its VRAM — so
   it can only mean an integrated part wearing that family's architecture id.
   Without it, a GB10-class board that reports Blackwell *and* happens to answer
   every domain would be treated as a validated discrete card and warn about
   nothing: the one path by which untested silicon could reach a user in
   silence. `gpu_capability_topology_contradicts_discrete()`; pinned both ways
   by regression codes 2190–2202.

Trigger 2 can fire with **nothing** missing, so the dialog has two texts — the
"reduced control surface … does not expose: (none)" phrasing would be nonsense
there. `integratedOnly` selects the integrated-part wording instead.

The second tier is informational only (no abort path) and lives in
`source/main_capability_warning.cpp`, which also owns
`show_gpu_support_warnings()` — the single GUI startup call site for both.

**Memory topology.** Windows reads the dedicated/shared split from DXGI
(`IDXGIAdapter1::GetDesc1`); Linux reports `UNIFIED` on an integrated-SoC
platform via `linux_platform_is_integrated_soc()` (the single Tegra/L4T
detection, shared by the probe report and the daemon). Classification is the
pure `gpu_memory_topology_from_sizes()` with a deliberately low 256 MiB floor,
so no discrete board can be misread as unified. Any failure stays `UNKNOWN`.
Inferring topology from the absent memory-offset domain was rejected: an older
driver or a refused read would then masquerade as a topology fact.

Matching DXGI to the selected adapter needs normalization — NvAPI reports a
**packed** identifier (device in the high word, vendor `0x10DE` in the low word,
e.g. `0x2F0410DE` for an RTX 5070) while DXGI reports the bare 16-bit device id.
Comparing them directly never matches and silently left every topology
`UNKNOWN`.

`confirm_unified_memory_write()` (`ui_main_apply.cpp`) gates a memory-clock
apply on positively-reported `UNIFIED` only, so no discrete GPU can reach it.
It is a confirmation, not a block — the write still proceeds if accepted.
The service projects the topology and all ten two-bit domain states through
the former reserved bytes of `ServiceGpuHealth`; `apply_service_snapshot_to_app()`
decodes them before GUI warnings and confirmations run. This keeps protocol v16
and the 216-byte health layout unchanged. An older v16 producer sends zeros,
which decode to `UNKNOWN` topology and `UNPROBED` domains.

## XBAR/SYS ClkDomains: version-keyed schemas (not family-gated)

Second aux clock domain (SYS): pinned entry index 3, verified by physical
CLK_MEASURE domain 2, shipped as protocol v21 with its own mutation bit and
portable key `sys_clk_offset_khz`. Entry mapping was established empirically
by `--clk-domain-probe` (entry 0 = GPC/measure 0, entry 1 = XBAR/measure 1,
entry 3 = SYS/measure 2). Video offset is pinned at entry 4 in protocol v22:
an exact before/after control-block diff changed only its standard `+0x114`
field, and the documented public clock-frequency query supplies live video
telemetry. Video has no CLK_MEASURE id; exact control-block readback is its
transaction proof. Do not blind-write non-offset dwords in the block.

Windows and Linux XBAR frequency/MSVDD control use the private **NvAPI
ClkDomains** surface, not Linux RM command IDs and not PropRels:

- GET `0xF58938F5`, SET `0xD14B69CF`, physical clock measure `0x527FC458`
  (version/size `0x0001000C`; XBAR domain 1, SYS domain 2)
- The interface is 10+ years old and its `.domains` sub-structure is
  **versioned per generation**. The driver's REPORTED
  version word — `(version << 16) | size`, read back at buffer offset 0 — must
  select the layout; a GPU-family whitelist is wrong by design. XBAR has been
  an independently controllable domain since Volta, so pre-Blackwell cards can
  work once their schema row is pinned from real hardware evidence.
- Exactly one schema row is validated today: version word `0x000261A4` (V2,
  R572..R610), eight repeated entries at base `0x124`, stride `0x304`. The two
  owned fields live in **different domain entries** (observed driver
  validation of the control block): XBAR frequency at entry 1's `+0x114` (kHz,
  buffer offset 0x53c), MSVDD voltage at entry 0's `+0x118` (µV, buffer
  offset 0x23c). Putting MSVDD in entry 1 (`0x544`, the old `+0x11c` guess)
  makes the RM driver reject SET with status 0xFFFFFFFF — that offset belongs
  to the SYS domain's second voltage rail, unsupported on Blackwell. The
  schema therefore carries a separate `msvddEntryIndex` alongside
  `entryIndex`. Rows live in the pinned `g_xbarSchemas[]` table; new
  generations slot in as additional rows keyed by their reported word, never
  as guessed offsets.

`source/gpu_backend_xbar.h` walks the table newest-first as a request ladder,
then dispatches on the RESPONSE's version word. An unknown word refuses all
access and logs decoded version/size words plus a bounded response-head dump —
that log line is the evidence a Volta..Ada owner can report to get their row
pinned. A tri-state snapshot status distinguishes `UNKNOWN_VERSION` (our table
cannot talk to this driver yet → capability UNPROBED, no limited-surface
warning) from `UNAVAILABLE`/refusals under a known schema (positive evidence →
REFUSED). Failed reads always clear `valid`; stale proof cannot survive.
It never synthesizes a full block: every write reads a fresh complete response,
changes only the two owned fields (absolute offsets carried in the snapshot),
submits that preimage, then requires exact readback of both fields. Reset writes
both owned fields to zero against another fresh read.

SET_CONTROL is a privileged operation: a non-elevated caller gets status
**-137 `NVAPI_INVALID_USER_PRIVILEGE`** before the RM command is dispatched
(GET_CONTROL works unprivileged — verified live). The status decoder names this
code explicitly so a non-elevated off-path tool is not misread as a driver
rejection of the layout.

Linux includes the same audited transaction through
`source/linux_backend_xbar.cpp`; it resolves the IDs through
`libnvidia-api.so.1`, carries XBAR/SYS/VIDEO values in the rollback snapshot,
and publishes them to the daemon/TUI only when the response schema and exact
readback validate. Reset selects optional phases only from domains proven both
readable and writable; an unknown or read-only optional surface does not
prevent core Reset.

PropRels (`GET_INFO 0xE826E4F0`, GET `0xCBFF71D0`, SET `0xEF3D20EA`) is the
separate GPC→XBAR propagation-ratio surface. Green Curve does not currently own
that ratio; confusing it with ClkDomains was the root cause of the first broken
implementation.

The capability probe (`source/gpu_capability_xbar_probe.h`) is one focused
read with NO family gate — availability is version-decided; it also clears the
previous probe's XBAR proof scalars first so selection/driver changes cannot
leave stale values. It performs no ID scanning and no writes.
The service refreshes offsets plus measured clock through
`source/xbar_telemetry.h`, so an out-of-band reset or external tool change is
visible instead of freezing the startup snapshot. Alongside clock, the service
and Linux backend poll active hardware voltage via `NvAPI_GPU_ClientVoltRailsGetStatus`
(`0x465F9BCFu`, version `0x0001004Cu`, `NvApiVoltRailsStatus` rail struct size 76 bytes,
`value_uV` at offset 40) using helper `xbar_measure_voltage()`. The measured voltage is
stored in `xbarMeasuredVoltageUv` and presented on the advanced page (formatted as `x.xxV`
alongside applied offset `%+d mV` on Windows GUI and Linux TUI). GUI capture emits both XBAR
fields when either changes; otherwise a voltage-only delta would preserve
voltage but reset clock (or vice versa). The synchronous manual-apply no-change
and fan-only shortcuts compare both captured fields against the service control
state; omitting that comparison let the orange pending domain reach Apply and then
wrongly report "No changes to apply". Requested XBAR on an unavailable surface
is a reported apply failure, not a silent skip. Explicit service Reset also
restores both fields to stock.

### XBAR profile/ownership contract

XBAR clock and MSVDD are two independently owned scalar fields:

- INI keys are `xbar_offset_khz` and `xbar_msvdd_offset_uv`. Windows parses them
  through `config_profile_xbar_io.h` and rejects out-of-range values rather than
  silently clamping a hand-edited record.
- A supported Windows profile save emits both effective values (owned value if
  present, otherwise current control/live state). On an unsupported surface it
  emits neither key. Loading projects an omitted key as explicit zero intent so
  a profile without XBAR restores stock rather than preserving whatever the
  previous profile owned.
- Sparse merging, active-intent ownership comparison, manual no-change/fan-only
  decisions, and named-profile identity all account for both fields.
- Named-profile transitions clean up exactly each previously owned field omitted
  by the next profile; partial declarations preserve the sibling.
- A portable profile carried to a GPU whose selected XBAR surface is unavailable
  has those fields stripped for that apply and cannot become active ownership.
  This keeps XBAR-specific records portable without turning every other
  domain into an XBAR failure.
- Service snapshots distinguish probe availability from fresh clock/MSVDD
  proof with two independent validity bits; failed telemetry keeps last-known
  editor values but clears both proofs. GUI control adoption is independent of
  fan-state presence.

Regression codes 4500–4524 pin the version-keyed transaction: fake-driver
full-block preservation, exact readback, mismatch refusal, decoy-marker
rejection, unknown-version-word refusal (no SET may fire), `-9`
INCOMPATIBLE_STRUCT_VERSION rejection, schema-table lookups, tri-state status,
and absolute field offsets. Transition codes 531, 532, and 4520 pin
cross-profile cleanup/preservation.
`tools/xbar_gates.py` pins the IDs/version, the schema-table design, the
family-blind probe, and forbids reintroducing PropRels or Linux RM IDs or
fixed-offset geometry recomputation. A live RTX 5070 read-only probe succeeded
post-rework with version word `0x000261A4`; pre-Blackwell behavior is
deliberately fail-closed-with-diagnostics until someone reports a real older-
generation response (no such local hardware exists). The privileged SET path is
structurally unit-tested but still needs an end-to-end hardware apply from an
installed rebuilt service.

## Windows `--self-test` (read-only pre-flight)

`source/main_self_test.cpp`, the counterpart of `linux_backend_self_test()`.
Runs in-process, mutates nothing, and needs **no background service** — it
dispatches before the service gate in `entry.cpp` on purpose, because its whole
job is to diagnose a machine where the normal path does not work yet.

It deliberately does **not** call the shared init helper: in the GUI binary that
resolves the VF curve through the service, so on a machine without a healthy
service it reports "VF curve read failed" and says nothing about the driver.
Instead it drives NvAPI directly (`nvapi_init` → `nvapi_enum_gpu` →
`nvapi_read_curve` → `nvapi_read_offsets`), which is also the only way to prove
the private pstates20 entry points resolve — they come back by ordinal, so no
export listing can establish it. Reports the loaded NVAPI module path (decisive
on arm64), elevation (an unelevated run yields `INCONCLUSIVE`, never a verdict
about the hardware), and the full per-domain surface. `FULL` requires NVML,
the VF curve read, the private control read, a writable VF domain, and a full
surface. Exit status is 0 only for `FULL`, 1 for unusable initialization or
enumeration, and 2 for partial, monitor-only, or inconclusive results.

The Linux self-test likewise captures the rollback hardware snapshot before it
prints capability and health, so the reported per-domain state is populated by
the same path used before a mutation.

The separate opt-in `--clk-domain-probe` is write-capable. Its
`GC_CLK_PROBE_ENTRY` / `clk_probe_entry.txt` selector is parsed by
`clk_probe_entry_policy.h` as a complete non-negative decimal entry within the
reported schema's domain count. Malformed or out-of-range text aborts the probe;
it must never inherit `atoi`'s invalid-text-to-zero behavior.

Measured on an RTX 5070 / x64: `Control surface: full`, every domain
`available`, topology `dedicated` — the live no-regression check.

## NVAPI module selection per architecture (`nvapi_module_policy.h`)

NVIDIA names the NVAPI runtime per machine architecture and a Windows-on-Arm
package ships every variant side by side. Verified against RTX Spark
developer-preview driver **616.00** (`Display.Driver\`):

| Image | Machine | Role |
|---|---|---|
| `nvapia64.dll` | ARM64 | the only one a native arm64 process can load |
| `nvapi64.dll` | x64 | emulation copy for x64 apps |
| `nvapi.dll` | x86 | emulation copy for 32-bit apps |
| `nvml.dll` | **ARM64** | native; needs no per-arch handling |
| `nvml_arm64ec.dll` | x64 | ARM64EC/x64 compat copy |

The historical `nvapi64.dll` → `nvapi.dll` order therefore finds only images of
the wrong machine type on arm64 and leaves the backend with **no NVAPI at all**.
The per-arch table fixes it; the x64 order is unchanged and pinned by regression
codes 2180–2188 (which also test the arm64 ordering from an x64 host, since it
is otherwise untestable without the hardware). Loading lives in
`source/nvapi_loader.cpp`.

### NvAPI entry-point id scan (settles the arm64 OC surface)

`nvapi_QueryInterface` is NVAPI's only export; every function Green Curve uses
comes back by 32-bit id from an internal dispatch table, so an export listing
cannot say whether the OC surface exists on a given build. `scan_nvapi_entry_point_ids()`
searches each image for those ids as little-endian literals. A specific 4-byte
value occurs by chance about once per 4 GB, so in an ~800 KB image a hit is
~0.02% likely at random; the full set hitting is effectively conclusive.

**Two architectures from the same package are each other's control.** Result on
driver 616.00 — every id, including all four private VF ids, present in the
ARM64 image with counts identical to x64:

| Entry point | arm64 | x64 | x86 |
|---|---|---|---|
| `NvAPI_Initialize` | 3x | 3x | 4x |
| `NvAPI_EnumPhysicalGPUs` | 2x | 2x | 4x |
| `NvAPI_GPU_GetArchInfo` | 2x | 2x | 4x |
| VF `getPstates20` / `getInfo` / `getControl` / `setControl` | 2x | 2x | 4x |

So the private VF-curve OC surface **is built into the arm64 driver**. What
still needs hardware is only whether the driver *honours* a write.

**Scan the `*_impl` images.** `nvapi64.dll` is an ~800 KB shim over a ~6 MB
`nvapi64_impl.dll`, and the dispatch table lives in the latter. An early version
excluded `_impl` on the unverified assumption that it merely duplicated the
shim; that made every id read MISSING in **every** architecture including the
x64 one Green Curve resolves successfully every day. That impossible result is
what identified the filter as the bug rather than the driver — the control pair
is what makes the scan self-checking, and a gate now pins `_impl` coverage.

**Hardware-free driver inspection** lives in `tools/driver_inspect.py`:
`--inspect-aarch64-driver` (Linux ELF, via llvm-nm) and
`--inspect-arm64-windows-driver` (Windows PE, via a built-in export-table
parser). The latter answers the question NVIDIA's RTX Spark developer preview
left open — whether an arm64 GeForce driver ships NVAPI at all. Extract the
setup `.exe` with 7-Zip first. A missing `nvapia64.dll` is decisive; a present
one is necessary but not sufficient, since the private pstates20 entry points
resolve by ordinal through `nvapi_QueryInterface` and cannot be proven without
hardware.

## VF backend dispatch (family → struct layout)

The backend selects at runtime based on detected GPU architecture through the shared `vf_backend_for_architecture()` mapping. Pascal, Turing, Ampere, Lovelace, and Blackwell are treated as tested known families. Only an unrecognized future architecture maps to `g_vfBackendFuture`, where `bestGuessOnly=true`; VF writes remain enabled by policy, but the GUI shows a best-effort warning that the user can disable with `[warnings] hide_unrecognized_gpu_warning=1`.

Windows metadata resilience: `nvapi_read_gpu_metadata()` logs NVAPI architecture and PCI query status, selected backend, and best-guess state. If a known adapter had a successful architecture/backend selection earlier in the same process and a later architecture read fails or maps to unknown while PCI identity still matches, the Windows backend keeps the last known backend for that same PCI adapter. This prevents transient metadata failures from making a known Blackwell/Ada/Ampere GPU appear as "best-effort VF write".

### Linux binding, health, and VF freshness

Linux resolves the NVML device and NvAPI handle as one binding before
publishing VF capability. Multi-GPU matching requires a unique bus/device match
with nonconflicting PCI device/subsystem IDs. Exactly one NVML GPU and one
NvAPI handle may use a logged sole-device fallback when those identifiers do
not conflict, even when their reported bus/slot differs. A null or ambiguous
handle never publishes VF capability. Cross-API device IDs are normalized by
locating NVIDIA vendor ID `10DE` in either 16-bit word (or accepting a 16-bit
device-only form); either NvAPI's documented internal device ID or external PCI
device ID may corroborate NVML. Compatible subsystem full/swapped/partial forms do not
create a false conflict. The backend retains NVML's stable raw representation
instead of replacing it with NvAPI formatting. A real conflict publishes both
raw IDs and whether device or subsystem comparison failed.

NvAPI architecture is queried once per matched handle and the result is
retained. If it is unavailable, `nvmlDeviceGetArchitecture` maps the documented
Pascal, Turing, Ampere, Ada, or Blackwell enum to the existing VF family. The
same exact PCI identity may retain a previously known backend across transient
metadata failure. Unknown future architectures still use the writable
best-effort backend, but family selection alone never authorizes a write.

A Linux VF snapshot is atomic: info, status, and control buffers are read into
temporary storage, their exact statuses and accepted versions are recorded,
and masks/counts, plausible ranges, and ordered voltage topology are validated
before publication. Live frequencies are intentionally not required to be
monotonic because legitimate applied curves can violate that property. Failed
refresh invalidates freshness and cached arrays cease to be authoritative. The
backend performs one immediate read-only re-enumeration/rebind/re-read for the
same PCI GPU; no sleeps or timing assumptions are used.

Typed `ServiceGpuHealth` distinguishes binding, architecture, and individual
VF-read failures. Its available-domain mask gates transactions before the
first write: independent fresh NVML controls may remain available while VF is
degraded, VF or mixed requests fail closed, and full Reset still requires every
domain. Rollback and VF writes require a fresh valid status/control pair.

Current Linux source anchors: `source/linux_backend_discovery.cpp`,
`source/linux_gpu_binding_policy.h`, `source/linux_gpu_selection.h`,
`source/linux_architecture_policy.h`,
`source/linux_vf_validation.h`, `source/linux_backend.cpp`, and
`source/linux_backend_mutation.cpp`. Last verified: 2026-07-16 through pure/
sanitizer coverage, cross-compilation of the native Linux fixture, x64/ARM64
checks, and full build 433; RTX 5080 hardware acceptance remains pending.

## GPU identity and selection

- NVAPI enumeration records up to `MAX_GPU_ADAPTERS` adapters in `GpuAdapterInfo`.
- The GUI selector shows ordinal, name, family, and PCI identity when available. It is placed in the top-right graph header gap, not in the VF edit/grid area.
- NVML device selection prefers PCI identity matching against the selected NVAPI adapter. On multi-GPU systems, if PCI identity is available but does not match any NVML device, the selection fails closed (`refusing ordinal fallback`). Ordinal fallback is only allowed on single-GPU systems or when no PCI identity is available for the selected GPU.
- The selected GPU is carried in `ServiceRequest::targetGpu`; the service reinitializes or rejects if the request target differs from the active hardware target.
- Config persistence stores the selected GPU under `[gpu] selected_index`; this is an ordinal fallback until a richer stable identity key is added.

## VF curve data model

- `VF_NUM_POINTS` = 128 points
- Each point: `VFCurvePoint { freq_kHz, volt_uV }`
- Visible points filtered by `MIN_VISIBLE_VOLT_mV` (700) and `MIN_VISIBLE_FREQ_MHz` (500)
- Point locking: flatten the curve tail after a chosen voltage anchor
- Global GPU clock offset can optionally exclude low-voltage points (`gpuOffsetExcludeLow70`)
- VF curve apply/save uses sparse user intent: only user-explicit points plus the locked tail are persisted or re-applied. Live NVAPI readback shifts on stock/pre-tail points are diagnostics, not desired curve state.
- Fan-only applies are partial hardware requests. They must merge into the service active desired state and preserve the existing sparse VF intent/masks instead of replacing the active desired state with fan-only data.
- The GUI graph/header display is intent-aware for locked profiles: points at and above the configured lock are drawn/summarized at the requested lock MHz and lock voltage, even if live NVAPI readback has temperature/boost drift in the hidden tail. This is display-only and does not trigger background VF reapply.
- **Drift-free owned-curve baseline (`g_app.appliedCurveMHz[VF_NUM_POINTS]`, build 350):** the single GUI-side source of truth for *owned* VF point MHz. Populated EXCLUSIVELY from intent (the applied / service-active `DesiredSettings`) via `capture_applied_curve_baseline()` (`main_runtime_gpu.cpp`), **never** from live `g_app.curve[]` readback. `0` = point not owned (show live). This extends the intent-aware display to **pre-tail** points, not just the locked tail. It drives: (a) fan-only apply detection in `capture_gui_apply_settings` (editor vs baseline, so expected boost drift on a pre-tail point can't reclassify a fan-only change as a curve edit → no spurious full reset-and-reapply mid-game); (b) `populate_edits()` owned-point display (so the graph/edit fields and any later save show intent, not drift — voltage/mV is immutable so still read live). Set on client apply success (only when `!fanOnlyApply`, so fan-only never drops the curve the service still holds), service active-desired adoption (`apply_service_desired_to_gui`), and startup; cleared on Reset (`reset_curve`). Guarded by `F-DRIFT-1` source checks in `build.py`. Intent-isolation only — it does NOT write the GPU to counteract drift, and as of 0.18 nothing does (the continuous drift monitor was removed — see "NO continuous VF drift correction" below).

## Apply pipeline

**2026-09-16 qualification:** the three holes this note used to warn about --
failures releasing the ceiling before the curve was proved safe, a refused batch
reporting success, and unpinned UV transitions being uncovered -- are fixed
(CT-02, CT-03, CT-05). What is still true is the broader caveat: the fixes are
proved against pure policies and source-text gates, and **no test executes the
real apply orchestration**, so the emitted call sequence and its partial-write
behaviour are unverified. See the current
[clock transition audit](clock-transition-audit.md) before generalizing either
the historical hardware acceptance below or the new deterministic coverage.

- `source/gpu_backend.cpp`: GUI/service dispatch, client fast path, and top-level apply wrapper
- `source/gpu_backend_apply.cpp`: service-side apply pipeline, including phase logging, VF curve application, rollback, and result reporting
- `source/gpu_backend_apply_ceiling.h`: the F-APPLY-CEILING transition clamp (`ApplyClockCeilingGuard`) and the peak-curve diagnostic
- `source/apply_clock_ceiling_policy.h`: the pure, cross-platform ceiling decision
- `source/main_fan_runtime.cpp`: fan apply helpers now used by the backend apply path
- `source/main_gpu_front.cpp`: rollback-to-safe-defaults helper for partial apply recovery

The apply flow still proceeds in phases:

1. Build sparse `DesiredSettings` from GUI state or profile
2. Validate fan curve if fan mode is curve
3. **Arm the transition clock ceiling** (F-APPLY-CEILING, below) before any
   clock-affecting write, whenever this apply could put the GPU above what it
   is already entitled to run. When protection is REQUIRED and cannot be
   installed, the apply refuses here with nothing written.
4. For GUI OC/UV/power applies, reset the OC baseline first when `resetOcBeforeApply` is set.
   **Critical ordering** (GPU offset first prevents TDR from tail-point frequency spike):
   - GPU clock offset (reset first — removes global boost before VF curve is touched)
   - Memory offset is retained until its direct incoming-target write
   - Power limit — **the target this apply will END at**, not the board default,
     and only when the request owns power, the board has a power control
     surface, and the board is not already at that target
     (`power_reset_before_apply_target_pct()` +
     `power_reset_before_apply_required()`); see the power-target section
   - VF curve offsets (reset last — tail points snap to factory base without dangerous boost)
5. Apply in phases (each logged via `set_last_apply_phase`):
   - GPU clock offset
   - Memory clock offset
   - VF curve points
   - Lock (NVML hard pin, or the locked-clock release that also hands over the
     transition clamp)
   - Power limit (skipped as already-satisfied when the reset phase wrote it)
   - Fan settings when included
6. Report per-phase success/failure with details

### F-APPLY-CEILING: the transition clock ceiling

**Invariant:** an Apply never raises the VF curve, and never lets a
reset-to-stock raise it, while the GPU is uncapped. Enforced since 2026-09-16;
see [clock-transition-audit.md](clock-transition-audit.md) for the eight
control-flow holes that used to break it and what remains unverified (no
hardware acceptance, no test executes the real apply orchestration).

**Source anchors:** `source/apply_clock_ceiling_policy.h` (the shared decision
and the measured incident), `source/gpu_backend_apply_ceiling.h` (Windows
guard), `source/linux_apply_ceiling.h` + `LINUX_MUTATION_LOCK_CEILING` in
`source/linux_transaction.h` (Linux),
`source/vf_offset_range_policy.h` (the single offset range the tail floor and
the batch refusal both read), `tools/apply_ceiling_gates.py`, regression tests
5200–5243 and 5400–5472.

The lock used to be the LAST clock write of an apply on both platforms. A
profile switch from an unpinned profile (FLATTEN holds its ceiling with the VF
tail floor, so `nvml_reset_gpu_locked_clocks` has already run) to a pinned one
therefore: dropped the old ceiling in reset-to-stock, settled 1 s at stock, wrote
the new curve with the full boost offset applied to every tail point — HARD mode
does not floor the tail, only FLATTEN does — and pinned the clock over a second
later. Measured 2026-09-13 under game load: tail readback 2947…3637 MHz for
1.21 s with nothing capping it, then `nvlddmkm` event 153. The full timeline,
and the HARD → HARD control case in the same log that did *not* crash, are kept
in the local-only log.

An NVML locked-clock ceiling is armed before the first clock-affecting write:

- **When.** Not "the request named a lock" — that misses the case where the
  danger comes from what is being LEFT. Protection is required when the request
  names a lock target, OR when a reset-to-stock will remove something holding
  the outgoing state down (a FLATTEN tail floor, a negative offset, an old
  pin). A purely positive outgoing offset needs nothing: the reset only lowers.
- **What value.** The LOWER of the two applicable entitlements. Outgoing is its
  pin if it had a HARD one, otherwise its live curve peak — never a pinned
  profile's curve peak, whose tail is deliberately raw and high (3637 MHz in
  the incident) and would cap nothing. Incoming is its requested lock target.
  `min(old, new)` violates neither intent and is what makes a low-pin →
  high-pin switch safe: the old pin holds until the new curve verifies.
- **Ceiling, not pin.** `SetGpuLockedClocks(0, ceiling)` caps without forcing
  the clock up at idle (what `nvidia-smi --lock-gpu-clocks=0,N` asks for). The
  `(ceiling, ceiling)` fallback adds a FLOOR, so it is used only when the
  request names its own lock; a clamp derived purely from the outgoing state
  uses the open-ended form or refuses.
- **Required ≠ available.** `required && !arm` refuses the transition before any
  mutation, naming the ceiling and the missing capability. It does NOT disable
  the write surface: fan, memory, power and every non-raising request still
  work, which is how unsupported and unprobeable GPUs keep working by default.
- **HARD** re-asserts the requested pin at the end — but only once the curve has
  verified, since the requested pin may be above the transition bound.
- **FLATTEN / unpinned** release at the existing `nvml_reset_gpu_locked_clocks`
  call, again only once the curve verified (`curveRequestOk || !curveTouched`).
  A failed curve retains the clamp instead.
- **Rollback and Reset** release only when recovery proved stock
  (`apply_recovery_permits_release()`). The earlier unconditional release
  silently undid the guard's own `retain()`, because rollback runs after it.

**Diagnostics:** `apply ceiling: plan arm=… ceiling=… finalPin=…`,
`apply ceiling: armed …` (must precede `curve batch pass 1 begin`),
`apply ceiling: clamp adopted / KEEPING / released`, and
`apply curve peak after batch: <peak> MHz at ci=N; transition clamp armed=…`
with an explicit WARNING when a peak exceeds the requested lock unclamped.

#### The apply-clock witness

`source/apply_clock_witness.h` (Windows) and `linux_apply_log_clock_witness()`
in `source/linux_apply_ceiling.h`. Exists because the first post-fix test run
could only report "nothing crashed" — which the pre-fix code also did most of
the time — and because that run turned out to have been **idle** (34–36 °C, no
game), discoverable only afterwards by inferring load from fan telemetry. A
crash that reproduces only under 3D load cannot be investigated with a trace
that does not record the load.

Samples the live GPC/SM/memory clock, GPU utilisation, board power and
temperature at `apply entry`, `ceiling armed`, `post-reset settle`,
**`post-curve-batch (pre-lock)`** and `post-lock`, plus one silent sample per
iteration of the existing `read_live_curve_snapshot_settled()` loop — the only
thing covering the middle of the ~175 ms window rather than just its ends. Ends
with one verdict line:

```
apply clock witness verdict: peak gpc=2940 MHz (at post-curve-batch (pre-lock))
  vs ceiling=2957 MHz armed=1 -> HELD; load peak util=97% power=243.1 W
  temp=61 C loadMeaningful=1 samples=9
```

- **Never samples from another thread.** A concurrent NVML reader alongside an
  in-flight NvAPI VF write is the exact class of racy behaviour under
  investigation; every sample is synchronous, between writes, on the apply
  thread. Held by a `forbid_text` gate on `CreateThread`.
- The verdict rule is pure and shared (`apply_clock_witness_verdict()`), so the
  platforms cannot disagree about what "held" means. `HELD (driver rounded up
  one bin)` is distinct from `HELD`: NVIDIA rounds a locked-clock request to a
  supported clock bin and the Blackwell VF table steps in 15 MHz
  (`APPLY_CLOCK_BIN_TOLERANCE_MHZ`). Anything beyond one bin is `EXCEEDED`.
- `loadMeaningful` (≥ 20 % GPU utilisation) is stated outright, and an idle run
  prints that a clean result does NOT clear the under-load case. Unknown
  utilisation is never upgraded to load.
- **Only samples the clamp could actually have capped are judged**
  (`apply_clock_witness_counts_toward_verdict()`): neither `apply entry` (before
  the clamp exists) nor `ceiling armed` (contemporaneous with the arming write,
  so still reading the pre-clamp clock) reaches the verdict. Both are reported
  as `preArmPeak=`, and every per-stage line carries `judged=0/1`. Both
  exclusions were found as live near-misses rather than by reasoning — five of
  eleven real runs were being judged on an unjudgeable sample, passing only
  because the outgoing clock happened to sit under the incoming ceiling.
- Requires the optional `nvmlDeviceGetUtilizationRates` /
  `nvmlDeviceGetPowerUsage`; a board without them logs the load as `unknown:`.

**Verified on hardware 2026-09-13** (Blackwell, four profile 3 ↔ 4 applies with
a game running, `loadMeaningful=1`, all `-> HELD`, no `nvlddmkm` event). The
decisive reading, at the instant that crashed the driver earlier the same day:

```
[post-curve-batch (pre-lock)] gpc=2925 MHz util=52% power=128.8 W temp=47 C
apply curve peak after batch: 3637 MHz at ci=125; clamp armed=1 at 2957 MHz
```

The VF curve read back **3637 MHz while the GPU ran 2925 MHz** under active 3D
load: NVML locked clocks do hold the GPC clock during a VF table rewrite on this
driver, which was the open question. This also removes the load dependence
rather than merely surviving it — the old failure was not "load makes an apply
crash at random", it was that the apply created a genuinely out-of-spec
operating point (3637 MHz on a board stable to 2957) whose lethality depended on
what the GPU was doing. Under the clamp that operating point no longer exists at
any moment of the apply.

**Every apply path inherits the guard**, because it sits at the top of the one
apply backend on each platform: boot/logon auto-restore, standby resume, driver-
crash recovery replay, tray/hotkey and auto-profile switches, GUI/CLI apply and
the installer settings transfer all route through
`apply_desired_settings_service()` (Windows) or `linux_backend_apply()` (Linux).
The boot path was confirmed empirically on 2026-09-13 after a reboot: the logon
auto-apply arms the clamp 1.13 s before its curve batch and ends
`-> HELD; loadMeaningful=0`. Boot is the least valuable case (the GPU is at
stock and idle); the paths that carry the original hazard are standby resume,
driver-crash recovery, and an auto-profile switch triggered by a game launching.

Also established: the driver accepts the open-ended `(0, ceiling)` form, so the
symmetric fallback is unused here; apply duration did not regress (standalone
applies got *faster*, 2.45 s vs 3.48 s, because the power write moved into the
reset phase).

**Cleared at full load the same day**: seven further applies at **99 % GPU
utilisation, up to 254 W** (a 250 W board at its cap) and **75 °C** — beyond the
48–63 °C of the crash window — all `-> HELD`. Heaviest run: the curve read back
3592 MHz at ci=126 while the GPU ran 2910 MHz. Across all seven the judged peak
stayed between 2902 and 2932 MHz, never within 25 MHz of the ceiling.

Two incidental confirmations from that data: one run's judged peak landed at
stage `curve settle`, in the *middle* of the post-curve window rather than at
either end — the case the settle-loop polling hook exists for; and
`post-reset settle` reads ~2677 MHz at 249.9 W, because the stock curve asks
more voltage per clock and the board power-throttles during the 1 s settle.
Expected, and harmless: clocks only fall there.

## Power target (TGP): the control surface, and what "unknown" means (F-POWER-SURFACE)

**Source anchors:** `source/gpu_backend_power.cpp` (read + write pair),
`source/control_readback_policy.h` (`power_limit_surface_available()`,
`power_limit_pct_from_mw()`, `power_reset_before_apply_required()`),
`source/gpu_core.h` (`POWER_LIMIT_DEFAULT_PCT`, `POWER_LIMIT_MIN_PCT/MAX_PCT`,
`validate_desired_settings_for_ipc()`), `source/gpu_backend_reset_baseline.cpp`,
`source/gpu_capability_probe.cpp`, `source/linux_backend_mutation.cpp`
(`linux_power_request_is_inert()`), gates in `tools/readback_gates.py`,
regressions 5040-5071 + 4756 in `tests/regression_main.cpp`.

### The unit, and why it can go missing

Green Curve expresses the power target as a **percentage of the board default**,
because that is the unit every surface speaks: the editor field, the profile's
`power_limit_pct` key, the IPC request, the reset-to-stock baseline, the
readback comparison. Producing one needs `powerLimitDefaultmW`. Three NVML calls
feed this domain and **all three are independently refused in the wild**:

| Call | Feeds | Refused when |
|---|---|---|
| `nvmlDeviceGetPowerManagementLimit` | `powerLimitCurrentmW` | notebook boards whose TGP the OEM/EC owns |
| `nvmlDeviceGetPowerManagementDefaultLimit` | `powerLimitDefaultmW` | same class; the symbol may also be absent |
| `nvmlDeviceGetPowerManagementLimitConstraints` | `powerLimitMin/MaxmW` | rarely — this one often answers alone |

### Invariants

1. **An unknown power target publishes `POWER_LIMIT_DEFAULT_PCT` (100), never
   0.** 100 names exactly the state Green Curve leaves an unwritable target in.
   0 reads as a real -100% request, and every consumer believed it (see below).
2. **`readback.powerLimit` / `powerLimitReadbackValid` is false whenever the
   percentage is not a measurement.** `power_limit_surface_available()` is the
   single predicate; `apply_control_readback_validity()` and
   `intent_readback_status.h` both call it rather than re-spelling it inline.
3. **Constraints are read first and unconditionally.** They drive the advertised
   OC range and the write-side bounds and stay meaningful when the limit read is
   refused. Reading them last made every consumer see `0/0/0 mW` on exactly the
   boards that need them.
4. **Either limit getter alone establishes a usable pair** (unknown default =
   current, unknown current = default). Requiring both *pointers* made a driver
   that simply does not export the default-limit symbol indistinguishable from
   one with no power surface.
5. **Ownership is never dropped.** `hasPowerLimit` stays true on both platforms
   even on a surfaceless board, so profile equality, the applied-profile tick,
   and active-intent recording are unchanged. Only the *write* and the gates in
   front of it are skipped.
6. **A non-default request on a surfaceless board still fails loudly**, by name:
   `Power limit N% cannot be set: this GPU's driver does not report a power
   target (constraints X..Y mW)`.

### The 2026-09-07 failure this section exists for

0.25.0 on an **RTX 3060 Laptop GPU**. Every Apply failed with `Reset before apply
failed: Power target did not reset`, with the power limit left at default, on a
machine whose VF curve was perfectly writable. The chain:

1. The limit read failed -> `powerLimitPct = 0`, all mW values 0.
2. The capability probe still said power-limit **available**, because it only
   exercised the constraints call — which this board answers.
3. The GUI showed **0** in the power field; in service mode
   `forceExplicitGlobals` makes every capture own the field, so `hasPowerLimit=1
   power=0` went out on every Apply.
4. `validate_desired_settings_for_ipc()` clamped 0 up to the 50% floor — the log
   shows the GUI capturing `power=0` and the service receiving `power=50`, a
   silent request to halve the board's power.
5. `reset_oc_before_gui_apply()` read `0 != 100`, called
   `nvapi_set_power_limit(100)`, which refused at `powerLimitDefaultmW <= 0`
   **silently**, and the Apply aborted before the VF curve was touched.

Not laptop-specific: it fires on any board that refuses the limit while
answering the constraints. Mobile support was not missing — one domain
fabricated a value and every consumer believed it.

### The reset phase writes the apply's own target (2026-09-13)

`reset_oc_before_gui_apply()` used to write `POWER_LIMIT_DEFAULT_PCT` and leave
the profile's real target to the very END of the apply, after the curve and the
lock. Power is an absolute percentage of the board default, not a delta, so
there is no stale baseline to clear — the only observable effect was that a
profile which LOWERS the power limit ran the whole apply (reset, the 1 s settle,
and the curve batch that raises the curve) at 100% TGP. The same 2026-09-13 log
shows it plainly: `read_power_limit: … -> 100%` at reset, `-> 85%` four seconds
later. `power_reset_before_apply_target_pct()` now supplies the target the apply
will end at, `power_reset_before_apply_required()` compares against that target
rather than the board default, and the apply phase counts the domain as
satisfied instead of issuing a second identical write. The user-facing failure
string changed from `Power target did not reset` to `Power target did not
apply`.

### Cross-platform parity

Linux refused the same request shape twice — the `unavailableDomains` gate and
`linux_backend_preflight()`'s "power limit cannot be snapshotted and written
safely" — and since a saved profile always carries the mandatory
`power_limit_pct` key, every profile apply was blocked there too.
`linux_power_request_is_inert()` exempts a board-default request on a
surfaceless board from both gates and from the write phase, keeping ownership.

### Diagnostics

Every failure path in this domain now names itself:

- `read_power_limit: nvmlDeviceGetPowerManagementLimit failed: <NVML status>`
  (and the same for the default-limit and constraints calls)
- `read_power_limit: power target unreadable on this board (constraints X..Y
  mW); publishing 100% as unknown, power writes will be refused loudly`
- `read_power_limit: current=… mW default=… mW -> …% (constraints …, curRet=…
  defRet=…)` on change
- `set_power_limit: refused pct=N - …` for all five write refusals
- `reset-before-apply: skipping power reset — no power control surface (…)`
- `populate_global_controls: power edit enabled=… (readbackValid=… current=… mW
  default=… mW)`

### One VF-info rule for the Linux probe and the Linux backend (F-02-001, 2026-09-15)

The private-NvAPI `getInfo` read — "give me the per-point editable mask and the
active clock count" — was implemented **twice**, and the two copies disagreed
about what a valid answer is:

| | `linux_backend.cpp` (live control) | `linux_gpu.cpp` (`--probe`) |
|---|---|---|
| all-zero mask | `LB_NVAPI_INVALID_DATA` | pre-seeded `0xFF`, treated as 128 editable points |
| `numClocks == 0` | `LB_NVAPI_INVALID_DATA` | substituted `defaultNumClocks` |
| `numClocks > 64` | rejected | not checked |
| bounds checks | once, up front | re-checked inline per `memcpy` |

On a GPU where `getInfo` succeeds but returns zeros — **exactly the
unrecognized-future-family case `--probe` exists to qualify** — `--probe`
reported a readable VF curve while the TUI/daemon refused the identical answer.
The diagnostic could not predict the control path, which is its only job, and
`README.md` tells users to run `--probe` before trusting write behaviour on a
new architecture.

Both now share `linux_vf_info_layout_fits()` and `linux_vf_info_data_usable()`
in `linux_vf_validation.h`, and the probe **refuses** the same answers the
backend refuses rather than papering over them: a probe that cannot predict the
control path is worse than no probe. Assertions 5315-5330.

This is the same shape as the fan-curve duplication recorded in
[testing.md](testing.md), where two copies silently diverged into a real shipped
defect. Same remedy: one rule, one place, asserted on both hosts.

**F-04-001, found alongside it.** `NvAPI_EnumPhysicalGPUs` writes at most
`NVAPI_MAX_PHYSICAL_GPUS` (64) handles and both call sites pass a 64-entry stack
array, but only `linux_backend_discovery.cpp` clamped the returned count —
`linux_gpu.cpp` checked `count < 1` and then used it as a loop bound. The clamp
existing at one of two identical call sites is what made it a defect rather than
a style difference. Both now ask `linux_nvapi_enum_count_is_usable()`
(assertions 5331-5336).

### Open question / stale-risk

We still do not know **which** NVML call the reported board refuses; the log
predates the per-call logging. The fix does not depend on the answer, and the
next report will say. *Unverified on hardware:* the fix is reasoned from the
user's log and covered by pure regressions, not reproduced on a laptop GPU here.

**Last verified:** 2026-09-16 for the F-APPLY-CEILING section (rewritten for
the CT-01..CT-08 implementation: required-vs-available, the min(outgoing,
incoming) ceiling, verified-curve-gated release, recovery that proves stock
before unlocking — all deterministic-tested, none hardware-accepted).
2026-09-13 for the reset
phase's power target (build + full regression matrix on this host; the transition
clamp is derived from the 2026-09-13 crash log and has NOT been re-tested by
repeating a FLATTEN -> HARD switch under game load). 2026-09-09 for the rest of
the power-target section (no laptop GPU available locally).

## Known constraints

- GPU state is machine-global; most recent apply wins
- NVML fan control requires the driver to support manual fan policy
- Fan curve runtime reasserts manual settings periodically, falls back to auto after repeated failures
- Clock offset ranges are queried from the driver at init time. The Windows GUI
  advertises them next to the fields, but only after intersecting them with the
  caps the apply path enforces (`+/-1000` MHz GPU and `+/-3000` MHz memory at
  the IPC boundary, `50..150 %` power) — see F-OC-HINT in
  [windows-ui-layout.md](windows-ui-layout.md). There is no driver-reported
  *percent* power range: it is derived from `powerLimitMinmW`/`MaxmW` relative
  to `powerLimitDefaultmW`.
- Memory clock offset ranges reported by NVML are advisory for explicit user/profile requests. The apply path preserves values such as `3000` MHz and attempts the driver write even if the reported range says `-2000..2000`; driver rejection is still surfaced as an apply failure.
- Runtime selective GPU offset persistence is only a hint. The GUI/service may reuse it only when the live VF offset pattern still matches the persisted offset and exclude-low count.
- Selective offset detection is gated by `g_app.lastApplyUsedGpuOffset`. When the last apply did not use a GPU offset (VF-only edits), detection is skipped entirely. The flag is set in all apply paths, propagated through `ServiceSnapshot`, and defaults to `true` on startup (so external tool offsets can still be detected).
- Selective offset detection also requires more than the two edited high pre-tail points when a locked tail is skipped, preventing explicit VF edits such as points 74/75 from being reported as a selective GPU offset.
- Locked tails are strict apply-time targets. The backend may tolerate non-tail cross-talk (shifts caused by large flatten offsets on adjacent tail points) for verification. Originally only non-explicit readback points were accepted; now explicit points within 5 VF points of a locked tail point are also accepted (cross-talk is strongest on immediately adjacent points). Explicit points far from the locked tail remain strict. The locked tail itself is always strict.
- **NO continuous VF drift correction (removed in 0.18).** There is deliberately no periodic "is the curve still exactly on target" monitor. NVIDIA's VF curve legitimately shifts a few MHz with temperature/boost; the old `service_check_active_vf_drift_monitor()` "corrected" it by re-applying the whole OC (reset-to-stock spike + aggressive rewrite) under game load — a **TDR risk** — and it looped forever whenever the flatten target sat below the driver's reachable floor (e.g. requesting 2957 while the −1000000 kHz floor bottoms out at 2962: a permanent 5 MHz "drift" it could never close, so it reapplied every window indefinitely, implicated in a real TDR 2026-07-04). We now **live with** the drift; the tail-drift telemetry (`main_tail_diagnostics.cpp`) is **diagnostic only**. Settings are re-applied only for an authenticated logon, a coalesced standby generation, or validated controlled driver recovery, through the single lifecycle worker and serialized lifecycle-apply boundary. Guarded by `F-NO-DRIFT-FIGHT` in `build.py`; see [automatic restore policy](auto-restore-policy.md).
- Runtime lock state is intent-first. When the background service has an active desired lock, `populate_service_snapshot()` reports that configured lock rather than a live-detected flat suffix, and live lock detection is suppressed instead of clearing or migrating the known lock.
- Service-side applies treat locks as request-scoped. A no-lock request cannot inherit stale service lock markers from a prior locked profile/apply.
- GUI fan-only applies keep the existing fan-only shortcut and do not reset, reapply, repaint, or de-sparsify OC/UV/power/VF settings.
- Restore-after-memory-write VF preservation failures are treated as apply failures and route through partial rollback.
- **Driver-refused VF points (build 353):** some VF entries are populated + marked editable in `vfMask` but are non-operating placeholders (e.g. a high-index point reporting ~400 MHz, below the 500 MHz visible threshold) that the driver silently pins at offset 0. `apply_curve_offsets_verified()` detects refusal (non-zero offset written, write succeeds, readback stays exactly 0) and accepts the point as non-offsettable instead of retrying it across a 2nd batch pass + a per-point fallback — each retry is a ~1s NVAPI `setControl`, so this was ~2s of wasted apply time (it pushed a selective-offset + flatten apply past the 5s client IPC timeout → "Timed out during reading service response", even though the apply succeeded). Accepting is a hardware no-op and mirrors the pre-existing verify-time acceptance (`gpu_backend_apply.cpp` "hardware refused offset … accepting actual", gated by `actualMHz >= 500`). It does NOT touch the flatten-tail correction loop. Guarded by `F-OFFSET-REFUSAL` + `F-DIAG-OFFSET` in `build.py`.

## Diagnostics

- Memory offset apply logs include display MHz, internal driver kHz, and NVML MHz values. If an explicit memory request is outside the reported range, the log says it is attempting the driver write anyway.
- Config/profile loading preserves out-of-range `mem_offset_mhz` values and logs that the reported range was exceeded instead of clamping them.
- Selective GPU offset diagnostics log when a persisted runtime request is ignored because it is non-selective or because live VF readback no longer matches.
- `current_applied_gpu_offset_mhz()` logs "last apply did not use GPU offset, skipping detection" when `lastApplyUsedGpuOffset` is false, indicating a VF-only apply bypassed detection.
- No-lock service applies log when stale lock markers are cleared after a successful clock/curve apply.
- GUI OC applies log the explicit reset-before-apply target.
- Fan-only applies log when the service merges them into active desired and when the GUI skips VF edit repaint to preserve sparse point intent.
- Telemetry logs sampled visible tail bookends and full-tail drift counts/ranges/max delta for active locks. Hidden/unpopulated VF endpoints are skipped so bogus endpoint data cannot dominate the drift max. Drift is diagnostic only: expected boost/temperature movement does not queue a service reapply. See [automatic restore policy](auto-restore-policy.md) for the limited real-event triggers.
- GUI graph paint logs when it hides live locked-tail readback drift behind the effective user lock display.
- Unknown-family diagnostics state that the fallback backend is best-effort but write-enabled. Known Pascal/Turing/Ampere/Lovelace/Blackwell families do not show the warning. Metadata logs include `archStatus`, `pciStatus`, architecture/implementation/revision, PCI identifiers, selected family/backend, and whether the same-PCI backend cache was retained.
- Fresh service initialization may diagnose a stale/non-effective NVML memory VF offset when PState/SMI cross-validation is unavailable. It logs `diagnostic only, no write`; service startup never "corrects" the register or otherwise mutates GPU state.
- The board maximum memory clock used by the memory-offset derivation (`pstateMemMaxMHz - nvmlMemMaxMHz`) is `nvmlDeviceGetMaxClockInfo(NVML_CLOCK_MEM)` for the selected device, re-read on every `detect_clock_offsets()` (2026-09-23). It replaced a hidden `nvidia-smi -q -d CLOCK` child with a redirected pipe, which took the **last** GPU's value on multi-GPU systems. Equivalence measured on the RTX 5070 host: NVML 14001 MHz == nvidia-smi "Max Clocks / Memory" 14001 MHz. `debug_log_on_change("nvml max memory clock: ...")` logs value and NVML status. The `getMaxClock` table entry had resolved the nonexistent export `nvmlDeviceGetMaxClock` (always null) until then. The `nvidia-smi -pl` power-limit fallback (plain child, no pipe) is unchanged.
- Large VF offset diagnostics are aggregated into one high-offset summary per curve batch. This preserves visibility for experimental tail-floor writes while avoiding dozens of repeated per-point warnings in normal locked-tail applies.

## Validation

### Offset bounds clamping diagnostic

`clamp_freq_delta_khz()` in `source/main_runtime_nvml.cpp:861` now logs when clamping occurs. Previously clamping was silent — the offset would be truncated to the driver range with no indication. Now each clamp event logs the original value and the clamped limit. This helps diagnose why an aggressive offset wasn't applied as configured.

### Voltage consistency diagnostic

`apply_desired_settings_service()` in `source/gpu_backend_apply.cpp` now snapshots per-point voltage at the start of the apply flow and checks for unexpected voltage drift (>10 mV) after the correction loop completes. Voltage is inherently immutable on NVIDIA VF tables, so any change indicates a driver state shift or NVAPI read instability. This is diagnostic-only (no correction applied).

### vfMask editability check

`nvapi_set_point()` in `source/gpu_backend.cpp:362` and `apply_curve_offsets_verified()` in `source/main_runtime_gpu.cpp:20` now check `g_app.vfMask` before writing each point. Points whose bit is not set in the vfMask (i.e., points that don't exist on this GPU) are silently skipped with a debug log. This prevents writing to phantom points.

## Tail drift: graduated versus uniform tail offsets (2026-05-19)

A controlled measurement on an RTX 5070 (Blackwell) compared two ways of holding a flat tail after 3D load + cooldown: the per-point graduated offsets Green Curve wrote at the time, and one uniform offset applied to every tail point.

### Test setup
1. Reset to stock via Green Curve, apply VF lock+flatten (2962 MHz @ point 76, points 74-75 pre-tail edits)
2. Capture VF curve via `--json` (all 128 points + offsets)
3. Apply 3D load, cool down, re-capture — tail drifted +8 to +15 MHz at higher voltages
4. Reset to stock, apply the same lock target using one uniform tail offset written directly through the driver (reference run)
5. Capture VF curve via `--json`
6. Apply same 3D load, cool down, re-capture — tail remained 100% flat

### Key findings

| Aspect | Graduated offsets (Green Curve at the time) | Uniform offset (reference run) |
|--------|-------------|-------------|
| Tail offset strategy | Per-point graduated offsets (e.g. +480000 at p76, -225000 at p126) | Uniform offset (-1052000 kHz) for ALL tail points from p77 to p126 |
| Tail drift after load | +8 to +15 MHz (47/51 points >2 MHz from target) | 0 MHz (0/51 points drifted) |
| Correction loop | 25-pass incremental correction based on live readback | Single uniform write (no correction loop) |
| Offset range limit | Hard-coded fallback ±300 MHz; uses driver-reported range | Writes through to driver regardless of reported range |

### Root cause analysis

The correction loop in `gpu_backend_apply.cpp` computes per-point offsets as:
`offset = targetMHz * 1000 - (liveFreq_kHz - currentOffset_kHz)`

For tail points at higher voltages (990-1240 mV), the stock curve's natural frequency is 3100-3200 MHz. To hold these points at 2962 MHz requires offsets of -200 to -240 MHz. However, the correction loop converges at apply time, but the driver gradually re-asserts its internal curve shape — the graduated offsets don't fully suppress the boost mechanism.

The reference run used a single massive negative offset (-1052000 kHz ≈ -1052 MHz) for all tail points. This value is far beyond the conventional range limits but the driver accepts it. The uniform offset overwhelms the driver's per-voltage curve adjustment, keeping the tail flat.

### Applied fix v1: Range limit increase (Build 107-108, FAILED)

Increased `FALLBACK_VF_OFFSET_LIMIT_KHZ` and reordered range priority to prefer GPU offset range (±1000 MHz). Added stuck/out-of-range diagnostic logging.

**Result: FAILED.** Testing on real GPU showed the range increase made the tail drift WORSE — from +15 MHz (2977 MHz) baseline to +30 MHz (2992 MHz) maximum. The wider range allowed the correction loop to write larger POSITIVE per-point offsets, which boosted tail frequency even more. This confirmed that the range limit was never the root cause.

### Applied fix v2: Uniform tail floor offset (Build 109)

**Root cause confirmed:** Per-point tail offsets are ineffective on Blackwell. The driver ignores individual tail-point deltas. Even with wider range limits, the correction loop computes different offsets for different tail points but the live frequency does not converge.

**Fix:** For ALL tail points beyond the lock point, apply a single uniform offset equal to `rangeMinKHz` (the driver's minimum supported offset, typically -1000000 kHz on RTX 5070). This follows the uniform-offset reference run.

Changes:
1. **Initial tail offset computation** (`gpu_backend_apply.cpp`): Lock point uses per-point computation; all other tail points use `floorTailOffsetKHz` (= range minimum).
2. **Correction passes** (`gpu_backend_apply.cpp`): Same uniform floor applied during correction iterations for tail points.
3. The range minimum (-1000000 kHz) is only 5.2% less negative than the reference run's -1052000 kHz, suggesting it should produce comparable tail-flatten behavior.

**Status:** Code written, builds clean, regression tests pass. GPU testing needed to verify the uniform floor produces a flat tail.

## NVML locked clocks APIs (hard clock pinning)

The installed `nvml.dll` exports APIs for true clock pinning (locking the GPU to a specific frequency, disabling all dynamic clocking):

| API | Signature | Min arch | Purpose |
|-----|-----------|----------|---------|
| `nvmlDeviceSetGpuLockedClocks` | `(device, minGpuClockMHz, maxGpuClockMHz)` | Volta | Lock GPU clock to [min, max] range |
| `nvmlDeviceSetMemoryLockedClocks` | `(device, minMemClockMHz, maxMemClockMHz)` | Ampere | Lock memory clock to [min, max] range |
| `nvmlDeviceResetGpuLockedClocks` | `(device)` | Volta | Unlock GPU clocks (restore dynamic) |
| `nvmlDeviceResetMemoryLockedClocks` | `(device)` | Ampere | Unlock memory clocks (restore dynamic) |

### Behavior

- Setting `min == max` pins the clock to that exact frequency (no dynamic scaling).
- Special symbolic values: `tdp` locks to TDP, `unlimited` allows full boost. `unlimited,unlimited` == reset.
- Requires **root/admin permissions** (the service already runs elevated).
- **Not persistent** across reboots or driver reloads (resets to default idle clocks).
- Coexists with VF curve offsets: the locked clock sets the operating frequency floor/ceiling, VF offsets still shift the voltage-frequency relationship at that frequency.

### Current project status

**Implemented (Build 265).** The `NvmlApi` struct has `setGpuLockedClocks`, `resetGpuLockedClocks`, `setMemoryLockedClocks`, `resetMemoryLockedClocks` fields, resolved via `nvml_resolve()` in `main_runtime_nvml.cpp`.

UI uses a tri-state lock checkbox: unchecked (no lock) → checkmark (flatten lock) → dot (hard lock/pin) → unchecked. The `LockMode` enum (`enum LockMode : int` — fixed underlying type so the IPC sanitizer can clamp hostile values without UB; `LOCK_MODE_NONE`, `LOCK_MODE_FLATTEN`, `LOCK_MODE_HARD`) flows through the full pipeline: `DesiredSettings`, `ServiceSnapshot`, profile INI (`lock_mode=`), service apply pipeline.

Checkbox interaction/visuals (Build 269):
- **Left-click** cycles NONE → FLATTEN → HARD → NONE.
- **Right-click** opens a context menu (`show_lock_context_menu`, `WM_CONTEXTMENU` in `ui_main_window.cpp`) to pick No lock / Flatten / Pin directly — switching FLATTEN↔PIN no longer requires cycling through NONE.
- **Glyphs** (`draw_lock_checkbox` in `main_shell.cpp`): FLATTEN = checkmark, HARD = filled dot.
- **Tooltip** (`create_lock_tooltips` in `ui_main.cpp`): explains the modes; comctl32 loaded dynamically (GUI does not link it), rebuilt with the checkboxes.
- **Input transaction** (`lock_checkbox_policy.h`, `ui_lock_checkbox.cpp`): only `BN_CLICKED` can advance, an armed mouse/Space gesture is consumed once, and an exact press-time lock-state stamp rejects startup/profile/service changes that occur before release. `WM_LBUTTONDBLCLK` plus its paired trailing release are inert after the ordinary first click. Every notification/decision and subclass-install failure is diagnosable.

Key implementation details:
- **NVML reset on mode transition:** the service apply pipeline calls
  `nvmlDeviceResetGpuLockedClocks` for any non-HARD apply that replaces the lock
  domain (`service_request_replaces_lock_domain()`), which is idempotent and
  handles HARD→FLATTEN, HARD→NONE, and stale locks. The reset runs in the
  service process (the GUI process does not own the NVML device handle).
  **Corrected 2026-09-13:** this entry previously said the reset happens "at the
  start of" a non-HARD apply. It does not, and never did — it runs *after* the
  VF curve batch, immediately before the final lock step. The distinction is the
  whole of F-APPLY-CEILING: between reset-to-stock and that call the apply is
  what removes the old ceiling, and until 2026-09-13 nothing replaced it until
  after the new curve was already written. Treat the phase log
  (`apply phase: …`), not this page, as the authority on apply ordering.
- **No tail offsets for HARD mode:** the VF curve tail is not flattened when in
  HARD mode — the NVML API pins the clock, making TAIL points irrelevant. Tail
  verification and monotonicity checks are skipped. **Corrected 2026-09-16
  (CT-06):** "tail" is the operative word. `verify_curve_request()` used to
  `return true` outright for HARD, verifying nothing at all — not the explicit
  pre-tail points the user typed, not the anchor. The pin says nothing about
  points BELOW the anchor, which run at their own voltages and are exactly where
  a bad undervolt destabilises a card. Pre-tail points are now checked.
  A corollary that matters for the transition: a HARD profile's raw high tail is
  safe ONLY because of its pin, so restoring that curve without the pin (Linux
  rollback, CT-07) is not restoring the same profile.
- **Lock mode persistence:** `lockMode` is propagated through `g_serviceActiveDesired` merge, snapshot adoption, profile save/load, and restart-reapply snapshots. `SERVICE_PROTOCOL_VERSION` bumped to 7. **Build 269:** lock state (`lockCi`/`lockMHz`/`lockMode`/`lockTracksAnchor`) is merged as a unit by `merge_desired_settings()`; previously the GUI **profile-save** path (`merge_desired_settings` + `capture_gui_config_settings`) dropped `lockMode`, so pinned profiles reloaded as flatten — fixed. See `llm-wiki/config-profiles.md`.
- **Pending-intent invariant (Build 272):** `g_app.lockMode != g_app.appliedLockMode` means the user holds pending, not-yet-applied lock intent (FLATTEN→HARD checkbox click, right-click menu switch, or a loaded profile at the same lock point). The snapshot lockMode sync in `apply_service_snapshot_to_app()` is gated on `lock_mode_sync_allowed()` (`app_shared.h`): it only adopts the snapshot's mode when the GUI is clean AND `lockMode == appliedLockMode`. Before this gate (root cause of the "pin not appliable" bug), the per-second telemetry snapshot — still carrying the previously APPLIED mode — silently reverted a HARD click to FLATTEN within ~1 s, so Apply reported "No changes to apply" and saves wrote flatten. All curve-detection paths (`sync_applied_lock_state_from_curve`) keep both fields equal, so detection never creates false "intent".
- **Backward compat:** old profiles without `lock_mode` default to `LOCK_MODE_FLATTEN` when `hasLock` is true. Profiles saved while the Build ≤271 clobber bug was active may contain `lock_mode=1` (flatten) where the user intended pin — re-save once to repair.

### Implementation notes (historical)

1. Add function pointer typedefs and fields to `NvmlApi` struct in `app_shared.h`.
2. Resolve via `nvml_resolve()` in `main_runtime_nvml.cpp` (e.g. `nvmlDeviceSetGpuLockedClocks`, `nvmlDeviceResetGpuLockedClocks`).
3. Map a VF curve point's frequency to `minGpuClockMHz = maxGpuClockMHz = targetMHz` for a hard pin.
4. UI needs a distinct "hard lock" mode vs the existing "flatten lock" (VF tail flatten).
5. Call `nvmlDeviceResetGpuLockedClocks` on unlock / reset-to-defaults.
6. **Interaction with VF offsets:** when both are active, the driver applies the
   VF offset to determine voltage at the locked frequency, so a VF point's
   frequency can be pinned while the VF curve undervolts it. **Answered on
   hardware 2026-09-13** (Blackwell, from the incident log rather than a
   deliberate test): the 13:42:42 apply wrote a full 128-point selective-offset
   curve batch *while* an `nvmlDeviceSetGpuLockedClocks(2957, 2957)` pin from the
   previous apply was still armed — the batch returned `ret=0`, converged, and
   the post-write VF readback and selective-offset detection were byte-identical
   to the unpinned case. Writing the VF curve under an active pin is therefore
   safe and is now the normal path (F-APPLY-CEILING). The pin clamps the
   *requested clock*; it does not alter the VF table the verification reads.
7. **Locked clocks versus flatten:** the captured curve data only shows VF tail flattening; any `nvmlDeviceSetGpuLockedClocks` use is a separate operation on top of the VF curve flatten.

Last verified: 2026-06-07 — confirmed `nvmlDeviceSetGpuLockedClocks` and `nvmlDeviceSetMemoryLockedClocks` exported from installed `nvml.dll` via `dumpbin /exports`. NVML docs confirmed at https://docs.nvidia.com/deploy/nvml-api/group__nvmlDeviceCommands.html.

## Current NVML lifecycle and stale-handle recovery

Stale NVIDIA user-mode handles after a selected-GPU removal, driver upgrade, or
TDR can access-violate or hang. Green Curve therefore does not attempt to reuse
the old process for a recovery write. The VEH remains a crash detector and
invalidates the stale NVML handle without calling `nvmlShutdown`; selected-GPU
Configuration Manager notifications and validated device-generation changes
provide corroborating recovery evidence. A global/null
`DBT_DEVNODES_CHANGED` notification is read-only and never authorizes a write.

Which images count as "the driver" (2026-09-23): the VEH and the crash
breadcrumb classifier use `gc_crash_module_is_nvidia_control_library()` in
`source/crash_artifact_policy.h`, matching any `nvapi*.dll` / `nvml*.dll` base
name. They used to match only the literals `nvml.dll`/`nvapi64.dll`, which
missed `nvapi64_impl.dll` (where the x64 NVAPI code actually runs) and every
ARM64 image (`nvapia64.dll`), so a stale-handle fault there crashed the process
instead of reaching this recovery. Stale-risk: not observed live on either
image; reasoned from the module layout in the NvAPI loader section.

A confirmed recovery with active in-memory intent starts the nonce-bound
controlled-restart protocol. The old service launches a minimal helper, commits
a dedicated clean exit, and performs no GPU write. The helper accepts only that
exit, pins the previous process identity, waits for SCM `STOPPED` through status
notifications, revalidates the protected nonce/snapshot, and makes one
`StartServiceW` attempt. Before reporting `RUNNING`, the fresh service validates
the nonce, old process, boot/freshness, target GPU, protected snapshot, and SCM
start reason. Only that validated fresh process may perform the one recovery
write.

Driver restore additionally requires a current-boot proof of 10 minutes of
awake stability (`QueryUnbiasedInterruptTime`) and recovery evidence below the
spam threshold. Recovery history and sticky lockout survive automatic success;
only a successful explicit GUI/CLI/hotkey/tray Apply clears them. Standby is a
different event: it replays complete in-memory intent once per suspend
generation without the 10-minute proof. Expected boost/temperature drift never
enters either path.

Ordinary service start, install/repair, Task Manager termination, crash, SCM
failure-action restart, stale/corrupt snapshot, or missing/wrong nonce is
non-mutating. A snapshot is intent, not authorization. The previous proof is
invalidated immediately before the first real hardware write; failure to commit
that invalidation aborts before mutation. See
[automatic restore policy](auto-restore-policy.md) for the complete contract.

Current source anchors:

- `source/main_crash_artifacts.cpp`: VEH driver-crash detection;
- `source/main_service_selected_gpu_pnp.cpp` and
  `source/selected_gpu_pnp_policy.h`: exact selected-adapter evidence;
- `source/main_service_controlled_restart.cpp`: nonce/helper/startup protocol;
- `source/main_service_recovery_clock.cpp` and
  `source/main_service_recovery_ledger.cpp`: awake proof and sticky history;
- `source/main_service_lifecycle_events.cpp`,
  `source/main_service_lifecycle_apply.cpp`, and
  `source/main_service_logon_coordinator.cpp`: reducer bridge and sole automatic
  write boundary;
- `source/main_service_persist.cpp`: protected intent/nonce/lockout state;
- `source/gpu_backend_apply.cpp` and `source/main_service_apply_runtime.cpp`:
  proof-at-write boundary and active ownership updates.

Current recovery architecture last verified: 2026-07-11. The build-numbered
material below is retained only as archaeology and must not be used as an
implementation contract.

## Historical recovery designs (not current)

> **HISTORICAL BUILD 228/230 DESIGN, superseded by the nonce-bound lifecycle architecture above.**
> After a GPU device reconnect / driver upgrade / TDR, the service does **not**
> attempt to reload `nvml.dll`/`nvapi64.dll` in-process. It snapshots the active
> profile, exits the process, the SCM failure action relaunches a fresh process,
> and the startup reapply restores the profile. **Build 230 fixed the SCM
> relaunch:** the recovery exit must terminate **without reporting
> `SERVICE_STOPPED`** so the SCM's default crash-recovery path fires the
> `SC_ACTION_RESTART` action (see step 4 below). The build-228/229 design reported
> `SERVICE_STOPPED`+non-zero exit and relied on a non-crash flag the LocalSystem
> service cannot set (`ERROR_ACCESS_DENIED`), so the service stayed dead after a
> reconnect. All the
> "Build 194–227 RC1–RC9" sections below describe the **removed** in-process
> recovery and are retained only as historical context. See
> **"Build 228 — recovery rearchitected to service-process restart"** below for
> the rationale and the current code map.

### Problem

When a GPU device is physically disconnected (e.g., eGPU unplugged, device removed via driver), calling `nvmlDeviceGetTemperature` on the stale device handle causes an **access violation inside nvml.dll**. This occurs despite the handle seemingly being valid at the API boundary — the internal driver state is corrupt/revoked while the library surface is still loaded.

### NVML DLL lifecycle

- `nvml.dll` is loaded once with `LoadLibrary` at service startup.
- After a driver reset, stale NVML/NvAPI handles can access-violate. `nvmlShutdown()` on the dead driver instance is unsafe (earlier builds observed hangs at 100% CPU).
- **Recovery is by process restart**, not in-process reload — in-process reload is fundamentally unreliable: the NVIDIA user-mode DLLs stay mapped (driver-pinned), so `FreeLibrary` does not unmap them and the new on-disk DLL is never loaded; `nvmlInit` then returns `ALREADY_INITIALIZED` on a handle bound to the dead driver instance, and NvAPI's process-global UMD cannot be reloaded and must version-match the kernel driver. A fresh process is the only state that reliably re-binds a version-matched UMD to the new driver.
- The **VEH** (`green_curve_vectored_handler`) stays as the crash **detector**: on a stale-handle AV it invalidates NVML via `service_close_nvml_without_shutdown()` (no `nvmlShutdown`), increments `g_nvmlCrashCount`, and `ExitThread`s the faulting thread so the process survives. The main service loop observes `crashCount>0` and calls `launch_recovery_thread()`, which now just `request_service_restart()`s.
- `nvml_ensure_ready()` still returns not-ready while `g_nvmlVhCrashed` / `nvml_crash_recovery_active()` is set (a brief pre-restart guard so other threads serve cached data). `g_serviceInitInProgress` remains a narrow bypass used by the post-restart **reapply thread** (not by any in-process reload, which is gone).
- **Invariant:** never call `nvmlShutdown()` from a stale-driver crash path or VEH handler. Use `service_close_nvml_without_shutdown()` and let a fresh process perform `nvmlInit_v2()`.

### Root cause

The `RegisterDeviceNotificationW` call was originally implemented with the wrong notification filter. It used `SERVICE_NOTIFY_STATUS_CHANGE` instead of `DEVICE_NOTIFY_SERVICE_HANDLE`:

- `SERVICE_NOTIFY_STATUS_CHANGE` only watches for service state transitions (start/stop/pause), not hardware device arrival/removal events.
- `DEVICE_NOTIFY_SERVICE_HANDLE` registers for Windows device interface notifications (GUID_DEVINTERFACE_GPU, etc.), which correctly deliver `DBT_DEVICEREMOVECOMPLETE` and `DBT_DEVICEARRIVAL` messages.

With the wrong filter, the service never received device removal/arrival notifications, so it kept calling NVML functions with the stale device handle from the original `nvmlDeviceGetHandleByIndex` call.

### Fix

1. **Correct notification filter:** Changed `RegisterDeviceNotificationW` to use `DEVICE_NOTIFY_SERVICE_HANDLE` with a `DEV_BROADCAST_DEVICEINTERFACE` filter structure, matching the documented pattern for hardware notification in Windows services.

2. **Defense-in-depth — `deviceRemoved` guards:**
   - Added a `deviceRemoved` state flag that is set when a `DBT_DEVICEREMOVECOMPLETE` notification is received.
   - `nvml_ensure_ready()` now checks `deviceRemoved` and returns an error instead of calling through to stale NVML functions.
   - `service_runtime_pulse()` (the main service loop) checks `deviceRemoved` and pauses NVML telemetry calls when the flag is set, preventing access violations during the disconnect window.

3. **Re-initialization on device arrival:**
   - When `DBT_DEVICEARRIVAL` is received, `deviceRemoved` is cleared and `launch_recovery_thread()` starts the dedicated in-process recovery thread. The SCM control thread must not call NVML/NvAPI directly.

### Driver-restart recovery (display-driver restart / driver upgrade) — restart the process

**Key invariant:** a display-driver restart utility and an in-place driver-upgrade install
restart the WDDM driver **without** sending `DBT_DEVICEREMOVEPENDING` /
`DBT_DEVICEARRIVAL`. So the device-notification path does **not** fire for the
common real-world case. The condition is instead detected by the **VEH catching
the stale-handle access violation** in nvml.dll/nvapi64.dll.

**Recovery = restart the service process.** Triggers and flow:
1. A recovery trigger fires: a VEH stale-handle crash (`g_nvmlCrashCount > 0`),
   `DBT_DEVICEARRIVAL` after a removal, a fan-pulse wedge (> 12 s), or an on-disk
   driver-version change.
2. All triggers route through the single chokepoint `launch_recovery_thread()`
   (`main_service_runtime.cpp`), which now only calls `request_service_restart()`
   (it kept the historical name because all call sites already call it).
3. `request_service_restart()` (`main.cpp`) is idempotent: it snapshots the active
   profile (`service_write_restart_reapply_snapshot()`), records the restart for
   loop protection (`service_record_restart_event()`), and signals the main loop
   to exit. It does **not** touch NVML/NvAPI.
4. The `service_main` loop's restart-exit branch terminates with `ExitProcess(1)`
   **without reporting `SERVICE_STOPPED`** and **without** the normal
   `nvml_set_fan_auto()`/`close_nvml()` teardown (those can hang on a dead driver).
   The SCM failure action (`SC_ACTION_RESTART`, 2 s/5 s/10 s, `dwResetPeriod` 10 min)
   relaunches us.
   **Critical invariant (build 230 — this was the build-228/229 bug):** the SCM
   queues `SC_ACTION_RESTART` *by default* **only** when the process dies *without*
   reporting `SERVICE_STOPPED`. Reporting `SERVICE_STOPPED` (even with a non-zero
   `dwWin32ExitCode`) looks *graceful* and is restarted only if the service opted
   into `SERVICE_CONFIG_FAILURE_ACTIONS_FLAG` /
   `fFailureActionsOnNonCrashFailures = TRUE`. That flag **cannot be set by the
   running service** — the `LocalSystem` token lacks `SERVICE_CHANGE_CONFIG` on its
   own service object (only `BA`/Administrators have `DC` in the default service
   DACL), so `ChangeServiceConfig2()` returns `ERROR_ACCESS_DENIED` at startup. So
   the recovery exit must NOT report `SERVICE_STOPPED`; it just terminates and lets
   the SCM's default crash-recovery path fire — needing only the
   `SC_ACTION_RESTART` actions, which are set at **install** (elevated/admin).
   - The failure actions + the (secondary, defense-in-depth) non-crash flag are
     configured by `service_configure_failure_actions()`, called at install AND at
     every service start by `service_ensure_failure_actions_configured()` — but the
     startup call is best-effort and silently `ERROR_ACCESS_DENIED` for LocalSystem;
     **install is the authoritative place.** Upgrading an already-installed service
     therefore requires a (re)install to pick up failure-action changes.
   - A normal/user stop is exempt: the graceful-shutdown path reports
     `SERVICE_STOPPED` with `dwWin32ExitCode = NO_ERROR`, so even with the flag set
     it is never auto-restarted.
5. The fresh process maps clean (possibly new) driver DLLs and the startup
   coordinator waits for the driver to be ready, logs the loaded
   `nvml.dll`/`nvapi64.dll` versions, then either re-applies the restart snapshot
   or resets before reconciling the active session profile.
6. **Restart-loop protection (persisted):** `service_record_restart_event()` /
   `service_count_recent_restarts()` keep a small on-disk ring of recent restart
   ticks (`%LOCALAPPDATA%\Green Curve\service_restart_history.bin`; for the
   LocalSystem service this is the SYSTEM profile's LocalAppData — see
   `resolve_service_machine_data_dir()`). If a fresh
   process sees ≥ `SERVICE_RESTART_LOOP_THRESHOLD` (5) restarts within
   `SERVICE_RESTART_LOOP_WINDOW_MS` (5 min), startup reapply goes **DORMANT**
   (F-REL-2): it skips re-applying to break the snapshot→apply→crash→restart loop
   but **retains the snapshot** (the user's profile is not discarded) and keeps the
   restart history. Re-arm is automatic: either the 5-min window ages the recent
   restarts out (driver settled) so the next fresh process reapplies, or a genuine
   `DBT_DEVICEARRIVAL` (driver (re)install) calls `service_clear_restart_history()`
   to force re-arm on the next process. The user can also apply manually via the
   GUI while dormant. An in-memory counter cannot do this because it resets on
   every restart. VEH minidumps are rotated (`service_rotate_minidumps(10)` at each
   service start) so a sustained loop cannot fill the disk.

`nvml_crash_recovery_active()` (`main.cpp`, 15 s window) still gates
SNAPSHOT/TELEMETRY/APPLY/RESET handlers and `hardware_initialize()` to serve
cached data / reject writes during the brief pre-restart transitional window.

**Traps (do not reintroduce):**
- Do NOT call `nvmlShutdown()` / `nvml_set_fan_auto()` / `close_nvml()` on the
  restart-exit path or in the VEH — they can hang at 100% CPU on a dead driver.
- Do NOT do blocking work in the SCM `service_control_handler_ex` device-event
  handlers — they run on the SCM control thread; just set flags / request restart.
- Do NOT reintroduce in-process NVML/NvAPI reload for driver-change recovery — it
  cannot pick up the new driver (DLLs stay mapped; UMD/KMD version mismatch).

### Build 228 — recovery rearchitected to service-process restart

**Symptom:** after a GPU device reconnect / driver-upgrade install (which replaces
all driver files), the service could no longer talk to NVML/NvAPI and lost all
functionality. Applying the OC profile **without** such an event was 100% stable.

**Root cause (independent of the prior project history, which corroborates it):**
in-process reload of `nvml.dll`/`nvapi64.dll` cannot recover a driver change.
The NVIDIA user-mode DLLs are driver-pinned/threaded, so `FreeLibrary` does not
unmap them and the follow-up `LoadLibrary` returns the **old** image; `nvmlInit`
then returns `ALREADY_INITIALIZED` on a handle bound to the dead driver instance.
NvAPI (which performs the VF-curve writes) is entangled with the process-global
NVIDIA UMD stack (DXGI/D3DKMT → `nvldumdx`/`nvwgf2umx` …) which is a process-wide
singleton that cannot be force-reloaded and must version-match the kernel driver.
A **fresh process** is the only state that reliably re-binds a version-matched UMD
to the new driver — i.e. the known-stable fresh-boot path. (Builds 182/183 reached
the same conclusion; build 186 reverted to in-process for UX, and RC1–RC9 never
made it work.)

**Change:** removed the entire in-process recovery (`service_recover_gpu_connection`
Phase A–E, `service_recovery_thread_proc`, `service_safe_close_nvml/nvapi`, the
recovery-thread hang/heartbeat watchdog) and routed every trigger to a controlled
**process restart** (see the flow above). The VEH stays as the crash detector; the
post-restart reapply machinery (`service_reapply_thread_proc` /
`service_check_reapply_thread_health`) and the snapshot/startup-reapply path are
kept (they re-apply settings on a healthy process). Added per-trigger version-delta
+ restart-decision logging and persisted restart-loop protection. `build.py` FP-06
checks were inverted to assert the restart contract; FP-08-001..004 (which tested
the removed in-process internals) were deleted.

**The "Build 194–227 RC1–RC9" sections below are historical** — they document the
in-process recovery that build 228 removed. Keep them only for archaeology.

### Build 194 — recovery wedge fix (in-process recovery no longer wedges after device reconnect or driver upgrade)

**Symptoms observed in build 193** (see `logs/greencurve_debug.txt`, 2026-06-01 16:12
session): after a device reconnect or driver-upgrade install, the service entered
a permanent recovery loop. `service_recover_gpu_connection()` logged "phase D:
re-initializing GPU state" but the next line was always `service_main: fan pulse
wedged for N ms`, with N growing by ~3 s per iteration (17766 → 20797 → 23813 → 26828
→ 29844 → 32859). The GUI continually saw `crash recovery active, skipping
hardware_initialize` snapshots and no apply/reset would work.

**Root causes** (all fixed in build 194):

1. **RC1 — Phase C cleared the safety guard that Phase D needed.**
   The old Phase C did `g_nvmlCrashCount = 0; g_nvmlCrashTickMs = 0;
   g_serviceGpuRecovering = 0;` so the recovery's own `nvml_ensure_ready()` could
   run. But Phase D's `hardware_initialize()` relies on `nvml_crash_recovery_active()`
   returning true to skip `refresh_global_state()` (which would wedge on a
   still-transitional driver). Clearing the guard made `refresh_global_state()`
   run, and it wedged.
   - **Fix:** Keep the broader crash-recovery safety guard SET throughout Phase C/D.
     Add a separate `g_serviceInitInProgress` flag so the recovery's own calls to
     `nvml_ensure_ready()` and `nvapi_qi()` bypass the safety-guard early-return.
     The guard is now only cleared in Phase E (after the reapply succeeds), at
     the same time `g_serviceInitInProgress` is cleared.

2. **RC2 — wedged recovery produced a ~3 s recovery-loop storm.**
   The wedge watchdog, main-loop monitor, and `service_runtime_pulse()` each
   independently called `launch_recovery_thread()` when a previous recovery
   wedged. With no cooldown, a single stuck recovery spawned a new one every ~3 s.
   - **Fix:** Add `g_serviceLastRecoveryAttemptMs` + `SERVICE_RECOVERY_RELAUNCH_INTERVAL_MS`
     (10 s) in `launch_recovery_thread()`. A second launch within 10 s logs
     "cooldown active" and returns. 10 s is well below the 15 s crash-recovery
     window so it does not suppress the legitimate single recovery on a fresh
     reconnect.

3. **RC3 — apply/reset failure wiped the active desired.**
   The old `service_apply_desired_settings` and `service_reset_all` unconditionally
   did `g_serviceHasActiveDesired = false; memset(&g_serviceActiveDesired, 0, ...)`
   on any failure. A single transient apply (e.g. mid-recovery) wiped the
   in-memory profile, so the next recovery had nothing to reapply and the service
   was stuck in "crash recovery active, skipping hardware_initialize" forever.
   - **Fix:** `service_apply_desired_settings` no longer clears the active desired
     on failure (logs "preserving active desired" instead). `service_reset_all`
     still clears on success but writes the disk snapshot on partial failure.
     `service_reapply_desired_preserving_intent` always calls
     `service_write_restart_reapply_snapshot()` as a safety net.

4. **RC4 — `FreeLibrary`+`LoadLibrary` may not pick up replaced on-disk driver files.**
   After a driver upgrade, the in-process `nvml.dll`/`nvapi64.dll` is the old
   version and the on-disk file is the new one. A plain `FreeLibrary` + `LoadLibrary`
   may return the old module because the loader's mapping table still references it.
   - **Fix:** Add `service_nvml_disk_version_changed()` /
     `service_nvapi_disk_version_changed()` that compare the in-process module's
     `GetModuleFileNameW` + `GetFileVersionInfo` against the on-disk
     `%SystemRoot%\System32\nvml.dll` / `nvapi64.dll` version. Called at the start
     of Phase B in `service_recover_gpu_connection` and in the `DBT_DEVICEARRIVAL`
     handler. When a version mismatch is detected, the log includes both versions
     so post-mortem analysis can confirm the upgrade happened. The actual reload
     is performed by the subsequent `service_safe_close_*` + Phase C `LoadLibrary`,
     which is sufficient for the in-place-replace case (the installer overwrites
     the on-disk file; the loader's mapping table reference is released by
     `FreeLibrary`). `version.lib` is now linked into both `greencurve.exe` and
     `greencurve-service.exe`.

**New globals** (declared in `app_shared.h`, defined in `app_shared.cpp`):
- `g_serviceInitInProgress` (volatile LONG): 1 while the recovery's own re-init
  is in progress (Phase C/D). Checked by `nvml_ensure_ready()` and `nvapi_qi()` to
  bypass the safety-guard early-return only for the recovery thread.
- `g_serviceLastRecoveryAttemptMs` (volatile ULONGLONG): timestamp of the most
  recent `launch_recovery_thread()` call. Used by the 10 s cooldown.

**Verification** (build 194):
- `python build.py` clean (no new errors or warnings; the pre-existing
  `__guard_check_icall_fptr` duplicate-symbol LTO warning is unchanged).
- `python build.py --test` passes all source regression checks (including the
  new RC1-RC4 checks under "FP-08-001").
- `python build.py --test --asan` passes all regression checks under
  AddressSanitizer.
- Manual verification deferred to a live device-reconnect / driver-upgrade test
  on a real machine.

### Build 195 — VEH / launch-side recovery cleanup (service survives a VEH crash of the recovery thread)

**Symptoms observed in build 194** (see `logs/greencurve_debug.txt`,
2026-06-01 17:23–17:25 session, after the RC1–RC4 fixes shipped): the
recovery loop is gone, but the service is still permanently wedged
after a single VEH crash. Log excerpt:

- `17:23:53.725` — `launch_recovery_thread: creating recovery thread (lastAttemptMs=0)` (tid=4076)
- `17:23:53.728–17:23:53.991` — Phase A, B, C succeed; `nvml_ensure_ready() succeeded on attempt 1`; `hardware_initialize() succeeded (populated=128)`
- `17:23:54.000` — `service_apply_desired_settings: interactive=0 gpu=0 exclude=0 mem=3000 power=100 fanMode=2 lockCi=76 lockMHz=2962 curvePoints=53` — last log line from the recovery thread before it dies
- `17:23:54.000–17:23:58.747` — 4.7 s silence. Recovery thread blocked inside `apply_desired_settings_service` (a `Sleep(1000)` floor plus a 25-pass curve correction loop with NVAPI `setControl` / `nvml_set_clock_offset_domain` calls).
- `17:23:58.747` — `service_runtime_pulse: new VEH crash detected, launching in-process recovery (crashCount=2)`. A VEH crash on a *different* thread (fan runtime / main loop) fires; the VEH `ExitThread(0)`s the crashing thread. The check at `main_service_runtime.cpp:751` finds `g_serviceGpuRecovering != 0` (set by the old, now-dead recovery thread) and **skips forever**.
- `17:23:58.747+` — `launch_recovery_thread: recovery already in progress, skipping` every ~1–2 s. `crashCount` increments 2 → 3 → 4 → 5; no new recovery is ever launched. The 10 s cooldown (RC2) is correct but never reached because the stuck-flag check fires first.

**Root cause** (new in build 194's new log evidence, fixed in build 195):

The VEH in `main_crash_artifacts.cpp` `green_curve_vectored_handler` is a
process-wide VEH that `ExitThread(0)`s **any** thread that access-violates
inside `nvml.dll` / `nvapi64.dll`. It does not know which thread it is
killing, and it does **not** touch the recovery flags
(`g_serviceGpuRecovering`, `g_nvapiRecoveryInProgress`,
`g_serviceInitInProgress`). If the VEH kills the recovery thread
mid-Phase-D's `service_apply_desired_settings()` call, those three flags
are stuck at 1 forever, and the `g_serviceRuntimeLock` mutex is
abandoned. No code path clears the stuck flags after a VEH kill. The
service is permanently wedged in "recovery already in progress, skipping"
state — only a process restart recovers it.

The apply path is the longest-running thread in the service on a
still-transitional driver: a `Sleep(1000)` floor at
`gpu_backend_apply.cpp:62`, plus an up-to-25-pass curve correction
loop, plus unbounded NVML/NVAPI readback/verify loops. With Phase D
holding `g_serviceRuntimeLock` and `g_serviceGpuRecovering=1` for the
entire 2–15+ second apply window, a second VEH crash on a different
thread is statistically likely to land inside the same apply window —
and the recovery thread is killed by `ExitThread(0)` with the flags
stuck.

**Three fixes** (build 195):

1. **RC5a — VEH clears stuck recovery flags when it kills the recovery thread.**
   Add two new globals to `app_shared.h` / `app_shared.cpp`:
   `g_serviceRecoveryThreadId` (volatile DWORD, mirroring the existing
   `g_fanRuntimeThreadId` pattern at `main.cpp:15`) and
   `g_serviceRecoveryThreadHandle` (volatile HANDLE). In
   `green_curve_vectored_handler`, after the existing
   `g_nvmlCrashCount++` / `InterlockedExchange(&g_nvmlVhCrashed, 1)`
   block and before the `ExitThread` pivot, add a
   `GetCurrentThreadId() == g_serviceRecoveryThreadId` check that
   clears all three stuck recovery flags and the handle. All three
   stores are `InterlockedExchange` (lock-free, VEH-safe). The
   log line `VEH: recovery thread (tid=N) killed, cleared stuck
   recovery flags` confirms the cleanup ran. `g_serviceRuntimeLock`
   is still abandoned by the dead thread, but
   `lock_service_runtime()` already handles `WAIT_ABANDONED`
   correctly (resets owner-TID and depth to the new owner at
   `main_service_runtime.cpp:952-964`).

2. **RC5b — `launch_recovery_thread` defensively detects a dead previous recovery thread.**
   The recovery thread's handle is no longer `CloseHandle`'d
   immediately after `CreateThread`; it is stored in
   `g_serviceRecoveryThreadHandle` so the next launch can detect a
   dead previous recovery thread. In `launch_recovery_thread()`, before
   the `g_serviceGpuRecovering` early-return, do a non-blocking
   `WaitForSingleObject(g_serviceRecoveryThreadHandle, 0)`. If
   signaled, the previous thread is dead — close the handle and
   clear the three stuck flags defensively. This is the backstop for
   any non-VEH kill (e.g. a future `TerminateThread` by a watchdog,
   or a crash in a non-`nvml.dll`/`nvapi64.dll` DLL that the VEH
   did not handle). The recovery thread's normal-exit path clears
   `g_serviceRecoveryThreadId` (the handle is closed lazily by
   either the next `launch_recovery_thread` call or RC5a's VEH-side
   cleanup).

3. **RC5c — Phase D defers the reapply to the queued reapply worker.**
   Replace the inline `service_reapply_desired_preserving_intent()`
   call in `service_recover_gpu_connection()` Phase D with
   `service_queue_recovery_reapply("driver recovery",
   NVML_CRASH_RECOVERY_WINDOW_MS + 1000)` (16 s). The queued reapply
   worker (`main_service_runtime.cpp:190`) already exists, already
   has timeouts + retries, and runs in the main service loop context
   where a VEH crash on any thread (fan runtime, main loop, the
   worker itself) cannot kill the recovery thread. The 16 s delay is
   the `NVML_CRASH_RECOVERY_WINDOW_MS` (15 s) safety window plus 1 s
   margin — the worker bails on `nvml_crash_recovery_active()` for
   the first 15 s, so the first successful attempt lands at 16 s
   when the safety window has just expired. If the desired was
   loaded from the disk snapshot (no in-memory `g_serviceHasActiveDesired`),
   the recovery thread promotes it to in-memory before queueing so
   `service_queue_recovery_reapply()` does not bail on the
   `!g_serviceHasActiveDesired` early-return. Behavior change: settings
   reappear ~16 s after a device reconnect instead of inline, but
   they appear reliably (today: appear immediately or never).

**New globals** (declared in `app_shared.h`, defined in `app_shared.cpp`):
- `g_serviceRecoveryThreadId` (volatile DWORD): TID of the live
  recovery thread (0 if none). Set by `service_recovery_thread_proc`
  on entry, cleared on normal return. Used by the VEH (RC5a) to
  detect when it is killing the recovery thread.
- `g_serviceRecoveryThreadHandle` (volatile HANDLE): HANDLE of the
  live recovery thread (nullptr if none). Set by
  `launch_recovery_thread` after `CreateThread` succeeds. Closed
  lazily by either the next `launch_recovery_thread` call (RC5b) or
  the VEH-side cleanup (RC5a) if a VEH crash lands first.

**Verification** (build 195):
- `python build.py` clean (no new errors or warnings; the pre-existing
  `__guard_check_icall_fptr` duplicate-symbol LTO warning is
  unchanged).
- `python build.py --test` passes all source regression checks
  (including the new RC5a/RC5b/RC5c checks under "FP-08-002").
- `python build.py --test --asan` passes all regression checks under
  AddressSanitizer.
- Manual verification deferred to a live device-reconnect /
  driver-upgrade test on a real machine.

### Build 197 — serialize recovery with Apply via runtime lock (in-process recovery's Phase B close is now lock-serialized with the pipe-server APPLY path)

**Symptoms observed in build 196** (see `logs/greencurve_debug.txt`,
2026-06-01 18:14–18:18 session, after the RC5a/RC5b/RC5c fixes shipped):
the recovery loop is stable, but **a setting-apply after a successful
recovery reapply still fails**, with the lock tail stuck at stock
2482 MHz instead of the requested 2962 MHz and a `Fan control change
failed: Failed to start fan curve maintenance` error. The user-facing
log (`logs/greencurve_log.txt` 18:16:25) reports:

- `Summary: Setting apply reported one or more failures`
- `Live GPU offset state: uniform 0 MHz`
- `Populated points: 128`
- `Lock tail verified at 2482 MHz @ 925 mV` (target was 2962 MHz)

Log excerpt (debug log):
- `18:15:01.464` — `service_apply_desired_settings: interactive=1 gpu=0 exclude=0 mem=3000 power=100 fanMode=2 lockCi=76 lockMHz=2962 curvePoints=53` — first Apply after a previous recovery succeeded; tail bookends settled at ci76=2962.
- `18:16:21.034` — `service_apply_desired_settings: interactive=1 gpu=0 exclude=0 mem=3000 power=100 fanMode=2 lockCi=76 lockMHz=2962 curvePoints=53` — second Apply starts; parameters identical to the first.
- `18:16:22.143` — `service recovery monitor: main loop launching in-process recovery (crashCount=3 pending=0)` — VEH crash on a different thread queues a recovery while the second Apply is still running.
- `18:16:22.145` — Recovery thread: Phase A (stop fan), then Phase B `service_safe_close_nvml: closing NVML (no shutdown)` + `service_safe_close_nvapi: closing NvAPI` — **both run WITHOUT `g_serviceRuntimeLock` held**.
- `18:16:22.190` — Apply (still holds the lock from the start of the request at `main_service_server.cpp:649`): `vf_curve_global_gpu_offset_supported: no backend or not writable`; `current_applied_gpu_offset_mhz: not Blackwell, returning NVML offset=0 kHz -> 0 MHz`; `apply_desired_settings: hasGpuOffset=1 gpuOffsetMHz=0 family=blackwell backend=<none> bestGuess=0 read=0 write=0`.
- `18:16:22.206` — Apply: `apply phase: apply: VF curve batch write`; correction pass computes `target=2962 MHz offset=480000 live=2482 offs=0` for ci=74..126 — `offs=0` for every point because the backend is gone.
- `18:16:22.364` — Apply: `apply curve: settled refresh failed after curve batch`.
- All 25 correction passes complete with `offs=0`; user sees `Lock tail verified at 2482 MHz @ 925 mV` and `Fan control change failed: Failed to start fan curve maintenance`.

**Root cause** (new in build 196's new log evidence, fixed in build 197):

The recovery thread's `service_recover_gpu_connection()` previously took
`g_serviceRuntimeLock` only at the **start of Phase C** (the re-init
phase). Phases A (stop fan) and B (close stale NVML/NvAPI handles) ran
**without the lock**. The pipe-server APPLY path takes the lock at
`main_service_server.cpp:649` and holds it across
`service_prepare_requested_gpu` + `service_apply_desired_settings` (the
apply takes 2–15+ seconds on a still-transitional driver: `Sleep(1000)`
floor at `gpu_backend_apply.cpp:62`, up-to-25-pass correction loop at
`gpu_backend_apply.cpp:741-934`, ~150-160 ms per
`read_live_curve_snapshot_settled(6, 25, ...)` at
`gpu_backend.cpp:825-852`, 5 s `WaitForSingleObject` on `nvidia-smi`
fallback, since moved to `gpu_backend_power.cpp`). Line numbers in this
historical appendix predate several shard splits — treat them as pointers to
the function, not to the line.

When a VEH-detected crash fires on a different thread during an
in-flight apply, `launch_recovery_thread()` is called and the recovery
thread tears down NVML/NvAPI under the apply's feet:

- The apply is mid-`service_apply_desired_settings()`, holding the lock.
- The recovery thread does NOT take the lock (Phase A/B are pre-lock).
- Recovery's `service_safe_close_nvml()` calls `FreeLibrary(nvml.dll)`
  and zeros the cached function pointers in `g_nvml_api`. The
  `nvml.dll` module mapping is gone.
- The apply, holding the lock, calls `vf_curve_global_gpu_offset_supported()`
  which checks the backend state and logs `no backend or not writable`.
- The apply's `read_live_curve_snapshot_settled()` calls return
  defaults; the apply's `apply_curve_offsets()` writes through NVAPI,
  which is also gone — every write silently fails.
- The correction loop runs 25 times, every time seeing `offs=0` (no
  backend), every write going to the closed NVAPI. Lock tail stays at
  stock 2482 MHz.
- The fan curve maintenance thread is started by Phase D / Phase E
  AFTER the lock is released, but the underlying NVML is gone, so
  `g_service.fanCurveRuntimeActive` is never set and the apply reports
  `Fan control change failed: Failed to start fan curve maintenance`.

The wedge watchdog in the main loop (`main_service_server.cpp:1017-1029`)
also had this same race: it `TerminateThread`'d the wedged fan thread,
then briefly checked the lock for orphaning, then **pre-emptively
called `service_safe_close_nvml()`** to free the stale module before
launching the recovery thread. That close also ran without the lock and
could race a concurrent apply.

**Three fixes** (build 197, RC6a + RC6b + RC6c/RC6d):

1. **RC6a — `service_recover_gpu_connection()` takes `g_serviceRuntimeLock`
   at the start of Phase A (before the Phase B close).** The lock is
   held through all of Phases A, B, C, D, E. The redundant
   `lock_service_runtime()` at the start of Phase C is removed
   (Phase C re-uses the Phase A acquisition). All early-return
   `unlock_service_runtime()` cleanup paths and the final Phase E
   `unlock_service_runtime()` are preserved unchanged. The function-
   level comment above the function is updated to reflect the new
   lock discipline and the function-level source regression check
   (RC6c) asserts the comment block references the RC6 fix.
   `stop_service_fan_runtime_thread()` (`main_service_runtime.cpp:1611`)
   already handles being called with the lock held (releases/retakes
   around `WaitForSingleObject` on the fan thread) — no change needed.

2. **RC6b — main loop wedge watchdog no longer pre-emptively closes
   NVML before launching the recovery thread.** The
   `service_safe_close_nvml()` call at `main_service_server.cpp:1027`
   (between the orphan-self-test pair and `launch_recovery_thread()`)
   is removed. The recovery thread (RC6a) now owns the NVML/NvAPI
   close in Phase B under the lock. The wedge watchdog keeps the brief
   `lock_service_runtime()` / `unlock_service_runtime()` orphan self-
   test (it correctly detects and the `WAIT_ABANDONED` handling at
   `main_service_runtime.cpp:952-964` resets the owner-TID and depth
   when a `TerminateThread` orphaned the lock). The main loop's
   pre-emptive close is now both redundant (the recovery thread does
   it) and racy (it ran without the lock, racing a concurrent apply).

3. **RC6c — FP-08-003 source regression checks** in `build.py`
   (added in the same commit):
   - **RC6c-1:** assert the function-level comment block above
     `service_recover_gpu_connection()` references the RC6 fix
     (regex match in a 12-line window).
   - **RC6c-2:** extract the function body (regex
     `r"static bool service_recover_gpu_connection\(\)\s*\{(.*?)(?=\nstatic\s|\Z)"`),
     count `lock_service_runtime();` calls **before** the first
     `service_safe_close_nvml();` (using a negative lookbehind to
     avoid matching the substring inside `unlock_service_runtime();`),
     and assert the count is exactly 1. The function legitimately has
     additional `lock_service_runtime();` calls **after** the Phase B
     close (the `nvml_ensure_ready()` retry loop's
     drop-during-Sleep re-acquire pattern at line ~631), so the
     "before close" window is the right invariant.
   - **RC6c-3:** assert the position of that single pre-close
     `lock_service_runtime();` is **before** the
     `service_safe_close_nvml();` position (i.e. the Phase A lock
     comes before the Phase B close, in source order).
   - **RC6c-4:** locate the wedge-watchdog block in
     `main_service_server.cpp` (regex
     `r"fan pulse wedged for[^\n]*\n.*?launch_recovery_thread\(\);"`)
     and assert it does NOT contain `service_safe_close_nvml();`.
4. **RC6d — hermetic compiled test for lock-serialization.** A new
   test case in the `fan_curve_regression.cpp` harness: two
   `CreateThread` instances against a fresh local `CRITICAL_SECTION`
   (not the production `g_serviceRuntimeLock`, which is created
   lazily by `ensure_service_runtime_lock()` and may be in an
   undefined state in a hermetic test). Each thread enters the CS,
   sleeps 200 ms, leaves. The test measures wall-clock time from
   `QueryPerformanceCounter` before `CreateThread` to after
   `WaitForMultipleObjects(TRUE)` with microsecond resolution (using
   `QueryPerformanceCounter` + `QueryPerformanceFrequency` because
   `GetTickCount` has ~15.6 ms resolution on Windows which would
   make the 400 ms threshold very flaky). Asserts:
   - `maxConcurrent` (peak count of threads simultaneously inside
     the CS) is exactly 1 (otherwise the CS is broken).
   - `finalCount` is 0 (both threads released the CS).
   - Total wall-clock time is >= 350 ms (400 ms expected — 2 * 200 ms
     sleeps — minus 50 ms slop for tick resolution and scheduler
     noise). Without serialization, the threads would overlap and the
     total would be ~200 ms, failing the threshold.
   This is a supplement to the deterministic RC6c source regression
   checks. Together they prove: (a) the production code's lock
   discipline has the correct shape (RC6c), and (b) the underlying
   lock infrastructure actually serializes (RC6d). The test adds
   ~400 ms to the regression test runtime.

**Verification** (build 197):
- `python build.py` clean (no new errors or warnings; the pre-existing
  `__guard_check_icall_fptr` duplicate-symbol LTO warning is
  unchanged).
- `python build.py --test` passes all source regression checks
  (including the new RC6a/RC6b/RC6c checks under "FP-08-003" and the
  RC6d lock-serialization compiled test).
- `python build.py --test --asan` passes all regression checks under
  AddressSanitizer.
- Manual verification deferred to a live device-reconnect /
  driver-upgrade test on a real machine.

### Build 198 RC7 — Recovery reapply moved to dedicated thread

**Problem:** After GPU device reconnect, the reapply ran on the main service
loop thread (`service_maybe_run_recovery_reapply`).  If NVML/NvAPI writes
crashed (transitional driver), VEH killed the main loop → service stopped →
SCM restarted → reapply never completed.  GUI falsely showed "settings active"
even though hardware was at stock.

**Fix (7 changes):**
1. `service_maybe_run_recovery_reapply()` rewritten as
   `service_maybe_launch_recovery_reapply_thread()` — creates a **dedicated
   thread** for the apply.  VEH kills only the reapply thread, not the main loop.
2. Added `g_serviceReapplyInProgress` flag + `ServiceSnapshot::serviceReapplyInProgress`
   — GUI shows "reapplying..." instead of misleading "settings active".
3. Phase C logs loaded NVML/NvAPI versions after forced reload.
4. Phase E re-registers `RegisterDeviceNotification` via
   `service_reregister_device_notification()`.
5. Disk snapshot is preserved on final reapply failure (safety net).
6. Reapply thread logs full intent, per-attempt results, and driver
   responsiveness.
7. Adaptive retry delay: 1s if NVML temperature read succeeds, 5s otherwise.

### Build 199 RC8 — Fix VEH-killed reapply thread, crash loop detection, Phase C retries

**Problems (build 198):**
After the reapply thread was VEH-killed during a transitional driver state (post
device-reconnect or driver-upgrade), two bugs left the service non-functional:

1. **`g_serviceInitInProgress` stuck.**  The VEH RC7 cleanup at
   `main_crash_artifacts.cpp` cleared the thread handle and TID but did **not**
   clear `g_serviceInitInProgress`.  The reapply thread had set it at
   `main_service_runtime.cpp:241` to bypass the crash-recovery guard; with it
   stuck at 1, `nvml_ensure_ready()` at `main_runtime_nvml.cpp:771` skipped the
   `nvml_crash_recovery_active()` check, disabling crash protection until the
   next recovery's Phase E cleared it.

2. **VEH-killed reapply thread treated as success.**  The VEH redirects crashing
   threads to `ExitThread(0)` → exit code 0.  The health check at
   `main_service_runtime.cpp:443` treated `exitCode == 0` as success and cleared
   `g_serviceReapplyInProgress`.  The retry-re-queue logic at lines 448-463 was
   inside `else if (exitCode != 0)` (line 445), so it **never ran** for VEH-killed
   threads — the reapply was silently dropped with no retry and no log trace.

3. **No crash loop limit on recovery cycles.**  The existing
   `record_driver_recovery()` / `count_recent_driver_recoveries()` /
   `MAX_RECOVERIES_BEFORE_BACKOFF = 3` mechanism (5-min window) was only called
   from `service_check_oc_persistence()` — the **TDR path** — not from the
   recovery flow itself.  The crash-recovery-reapply loop ran unboundedly.

4. **Phase C retry count too low.**  5 attempts × 500ms = only 2.5s for both
   `nvml_ensure_ready()` and `nvapi_init()` retry loops.

5. **Reapply thread early-exit paths leaked `g_serviceInitInProgress`.**  Two
   early-return paths (device removed, no desired settings) returned without
   clearing the flag.

**Fixes (7 changes, build 199 RC8a–RC8g):**

1. **RC8a — VEH reapply cleanup clears `g_serviceInitInProgress`**
   (`main_crash_artifacts.cpp`): Added
   `InterlockedExchange(&g_serviceInitInProgress, 0)` in the RC7 reapply thread
   kill block, right after clearing `g_serviceReapplyThreadId`.

2. **RC8b — Reapply thread early-exit paths clear `g_serviceInitInProgress`**
   (`main_service_runtime.cpp:210, 222`): Added the same clear before both
   early-return blocks.

3. **RC8c — VEH-kill detection in reapply health check**
   (`main_service_runtime.cpp:443-468`): Restructured the exit-code handling so
   `exitCode == 0` AND `g_serviceReapplyInProgress` still set means VEH-killed
   → retry instead of silently accepting.

4. **RC8d — Crash loop detection wired into recovery flow**
   (`main_service_runtime.cpp:757, 1054-1060, 169`):
   - `record_driver_recovery()` called at Phase A start (after lock acquisition).
   - Loop detection check before Phase D queues the reapply: if
     `count_recent_driver_recoveries() >= MAX_RECOVERIES_BEFORE_BACKOFF`, clear
     `g_serviceHasActiveDesired` and skip the reapply.
   - Defense-in-depth in `service_queue_recovery_reapply()`.

5. **RC8e — Phase C retry count increased from 5 to 10**
   (`main_service_runtime.cpp:922, 950`): Both `nvml_ensure_ready()` and
   `nvapi_init()` retry loops now try up to 10 attempts (5s total).

6. **RC8f — Extended reapply delay after repeated crashes**
   (`main_service_runtime.cpp:1055`): If `g_nvmlCrashCount > 1`, use 30s
   delay instead of 16s.

7. **RC8g — Temperature probe after Phase C** (`main_service_runtime.cpp:978`):
   Lightweight NVML temperature read after version logging to verify handles
   are functional. Diagnostic only — does not fail the recovery.

**Verification:**
- `python build.py --check` — both binaries compile cleanly.
- `python build.py --test` — all source regression checks pass (existing
  FP-08-001/002/003 + new FP-08-004 RC8a–RC8g).
- Manual verification: apply OC settings, trigger a display-driver restart, verify
  debug log shows recovery completes and settings reapplied.  If reapply
  crashes, crash loop detection fires after 3 recoveries in 5 min and breaks
  the loop.

### Historical Build 228 source map

- `source/gpu_backend.cpp` / `source/gpu_backend_apply.cpp`: VF-curve read/write, fan/clock operations, service-side apply flow, `lastApplyUsedGpuOffset` flag
- `source/gpu_backend_power.cpp`: the power-target read/write pair, split out of `gpu_backend.cpp` at its size ratchet and `#include`d from the position it occupied (see the power-target section)
- `source/gpu_backend_reset_baseline.cpp`: the reset-to-stock-baseline step every profile-switching Apply runs first
- `source/gpu_core.h` / `source/vf_backends.cpp`: `VfBackendSpec`, `GpuFamily`, `VFCurvePoint`, `DesiredSettings`, `ServiceSnapshot`, NVAPI/NVML type definitions, shared architecture-to-backend mapping
- `source/main_gpu_front.cpp`: Windows NVAPI metadata read/logging, backend selection, same-PCI last-known-backend retention
- `source/main_gpu_state.cpp`: Selective offset detection (`detect_live_selective_gpu_offset_state`, `live_selective_gpu_offset_matches_requested_shape`)
- `source/main_tail_diagnostics.cpp`: GUI-side full-tail drift diagnostics
- `source/main_state_sync.cpp`: `populate_service_snapshot`, `apply_service_snapshot_to_app`, `hardware_initialize`
- `source/main_crash_artifacts.cpp` `green_curve_vectored_handler`: VEH crash **detector** — invalidates NVML via `service_close_nvml_without_shutdown()`, increments `g_nvmlCrashCount`, `ExitThread`s the faulting thread; keeps the RC7/RC8a reapply-thread cleanup (build 228 removed the RC5a recovery-thread cleanup since the recovery thread is gone)
- `source/main_service_recovery.cpp`: startup coordinator and restart-snapshot reapply selection
- `source/main_service_runtime.cpp`: `launch_recovery_thread()` (now just `request_service_restart`), `service_reapply_thread_proc` / `service_check_reapply_thread_health` (event-driven reapply: post-restart / resume / logon retries — NOT drift), `service_nvml/nvapi_disk_version_changed` (version-change detect)
- (removed in 0.18) `source/main_service_vf_drift.cpp` — the continuous `service_check_active_vf_drift_monitor` is gone; no periodic drift correction exists
- `source/main.cpp`: `request_service_restart()` (snapshot + record + signal exit, no NVML), `g_serviceRestartRequested`
- `source/main_service_server.cpp`: `service_main` restart-exit branch (non-zero `ExitProcess`, no NVML teardown), `service_configure_failure_actions` / `service_ensure_failure_actions_configured` (SCM auto-restart at install + every start), device-event handler, fan-pulse wedge watchdog → `request_service_restart`
- `source/app_shared.h`: `ServiceSnapshot::serviceReapplyInProgress` — GUI-facing reapply status flag (build 198 RC7)

### Build 227 RC9 rev.2 — Phase C no longer makes device-level calls, recovery thread heartbeat watchdog

**Problem (RC9 v1):** The `setjmp`/`longjmp` crash guard did not work with the
llvm-mingw (MinGW) toolchain.  The VEH called `longjmp` but the recovery
thread still died (lock abandoned).  Additionally, the temperature probe is a
blocking kernel-mode call that can **hang** (not just AV) — VEH cannot detect
hangs.

**Revised approach:** Phase C now skips ALL device-level NVML/NvAPI calls
(temperature probe, nvapi_enum_gpu).  After loading libraries and initializing
sessions, it proceeds directly to Phase D (`hardware_initialize`).  A recovery
thread heartbeat (`g_serviceRecoveryHeartbeatMs` stamped before and after
Phase D) lets the main-loop watchdog detect hangs: if the heartbeat is stale
for > 30s, `TerminateThread` kills the stuck recovery thread, the abandoned
`g_serviceRuntimeLock` is recovered, and recovery retries.

**Fixes (build 227, RC9 rev.2):**

| Fix | File | Change |
|-----|------|--------|
| RC9a | `main_service_runtime.cpp:800-808` | (kept) Clear `g_serviceFanPulseInFlight` in Phase A |
| RC9d | `main_runtime_nvml.cpp:684-686` | (kept) Skip PCI matching during `g_serviceInitInProgress` |
| RC9e | `main_service_runtime.cpp:1035-1045` | Removed setjmp/longjmp guard, temperature probe, nvapi_enum_gpu from Phase C; added `g_serviceRecoveryHeartbeatMs` |
| RC9f | `main_service_server.cpp:1071-1102` | Added recovery thread hang watchdog in main loop (30s timeout, TerminateThread + flag cleanup) |

**Invariant:** Phase C is now lightweight — only `LoadLibrary`, `GetProcAddress`,
`nvmlInit_v2`, and `NvAPI_Initialize`.  Device-level calls happen in Phase D
where the heartbeat watchdog can detect hangs.

Last verified: 2026-06-04 for Build 228 — recovery rearchitected to service-process restart (removed all in-process NVML/NvAPI reload: `service_recover_gpu_connection` Phase A–E, recovery thread, hang/heartbeat watchdog; every trigger now `request_service_restart`s; safe non-zero restart-exit; SCM failure actions applied at every start; persisted restart-loop protection; FP-06 inverted to the restart contract, FP-08-001..004 deleted). The Build 194–227 RC entries below are historical (in-process recovery, now removed). 2026-06-03 for Build 227 RC9 rev.2 (Phase C skips device-level calls, recovery thread heartbeat watchdog, removed ineffective setjmp/longjmp). 2026-06-03 for Build 226 RC9 v1 (setjmp/longjmp crash guard — REVERTED in 227). 2026-06-03 for Build 199 RC8a–RC8g fixes (VEH reapply cleanup clears g_serviceInitInProgress, VEH-killed reapply thread detection in health check, crash loop detection in recovery flow, Phase C retry count increased, extended reapply delay, temperature probe). 2026-06-02 for Build 198 RC7 fixes (recovery reapply moved to dedicated thread, reapply-in-progress GUI flag, device notification re-registration, per-step apply logging, driver-health adaptive delay, disk snapshot preserved on failure). 2026-06-01 for Build 197 RC6a/RC6b/RC6c/RC6d fixes (lock-serialized Phase B NVML/NvAPI close, removed redundant wedge-watchdog close, FP-08-003 regressions + hermetic lock test). 2026-06-01 for Build 196 RC5a/RC5b/RC5c fixes (VEH-safe recovery flags, deferred reapply to worker). 2026-06-01 for Build 194 RC1-RC4 fixes (no-recovery-wedge, driver version detect). 2026-05-31 for driver-restart / device-reconnect recovery via in-process handle reload. 2026-05-20 for strict `nvidia-smi` parsing and aggregated high-offset diagnostics. Baseline scan files: `gc_baseline_curve.json`, `ab_curve.json`, `ab_postload_curve.json`, `gc_post_apply_20260519.json`.
