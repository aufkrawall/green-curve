# Changelog

## 0.24.0

Green Curve 0.24.0 brings advanced auxiliary-clock tuning, native zero-RPM
custom fan curves, and a more resilient Windows service.

### Highlights

- **Advanced clocks on Windows and Linux.** The new Advanced Clocks dialog on
  Windows and Advanced tab on Linux can tune XBAR, SYS, and VIDEO clock offsets
  plus XBAR MSVDD voltage, with live readback and full profile support. Controls
  are available only when the installed NVIDIA driver exposes a validated
  interface.
- **Native zero-RPM fan curves.** A custom curve can now hand fan control back
  to NVIDIA below your chosen temperature so supported cards can stop their
  fans at idle, then resume the custom curve above it. The fan start/stop gap
  and ordinary curve-downshift hysteresis are independently adjustable.
- **More resilient Windows service communication.** The service can now handle
  several local requests independently, so one stalled client no longer blocks
  GPU controls. Reconnects and user-session transitions are also more reliable.
- **Safer, bounded diagnostics.** Debug logs are capped automatically, and
  account, session, and path details are recorded as opaque fingerprints rather
  than plain text.
- **Profile, Linux, and update-path fixes.** Advanced-clock settings now
  round-trip through profiles on both platforms, reset and restore paths are more
  consistent, and updater failure recovery is more robust.

### Compatibility notes

- Existing profiles remain compatible. Native zero-RPM mode is opt-in.
- Advanced clocks have been validated on a Windows RTX 5070. Linux
  advanced-clock writes have automated test and build coverage but have not had a
  live-hardware pass.
- Native zero-RPM uses the GPU firmware's automatic fan policy. Whether the
  fans physically stop is board- and firmware-dependent, and the new mode has
  not yet had a live-board validation.

### Downloads and verification

- **Windows:** use the `setup.exe` for a normal install or upgrade; use the
  `.7z` archive for a portable copy.
- **Linux:** extract the `.tar.xz` archive and run the included
  `greencurve-setup.sh`.
- x64 and ARM64 packages are attached below. ARM64 remains compile- and
  binary-inspection-only; Windows x64 and Linux x64 are the hardware-tested
  targets.
- Every program package has a matching SHA-256 file and a GitHub
  build-provenance attestation. Verify an artifact with:

  ```bash
  gh attestation verify <artifact> --repo aufkrawall/green-curve
  ```

**Full changelog:** [0.23.1...0.24.0](https://github.com/aufkrawall/green-curve/compare/0.23.1...0.24.0)
