# Green Curve

Green Curve is a small Windows tool (will see if anything goes on Linux) for inspecting and editing the NVIDIA voltage/frequency curve on supported GeForce GPUs, mainly tested on RTX 5070. Has now also been user-tested on Lovelace, in addition to Blackwell support. Older GPUs *might* work, but are untested.

<img width="1772" height="1238" alt="greencurve" src="https://github.com/user-attachments/assets/ec2f8430-0eb9-47ca-919e-c7df4ef71775" />

Latest version can be found in tags section.

## What it does

- Reads the live VF curve from the NVIDIA driver
- Lets you edit visible curve points in a simple Win32 GDI UI
- Supports point locking to flatten the tail of the curve after a chosen voltage point
- Reads and writes global GPU clock offset, effective VRAM offset, power limit, and manual fan speed where supported
- Also supports custom fan curve mode running in system tray with background process
- Provides CLI modes for dump, JSON export, and probing driver capabilities

## Technical notes

- Single-file C++ Win32 application built with `zig c++`
- Uses NVIDIA driver interfaces available on the local machine
- Uses public NVAPI entry points exposed by the installed driver
- Uses NVML from the local NVIDIA driver install for supported management operations
- Optional debug logging exists, but normal release usage is intended to run without persistent logging output
- Tiny

## Build

```bash
python build.py
```

This produces `greencurve.exe`.

## Safety warning

This tool can change GPU clocks, voltage/frequency behavior, power limit, and fan control. These actions can cause instability, crashes, thermal issues, data loss, reduced hardware lifespan, or hardware damage. Manual fan control in particular can be dangerous if used carelessly.

Use it only if you understand the risks and are able to monitor temperatures, stability, and cooling behavior yourself.

## No warranty / liability

This project is provided under the MIT license, without warranty of any kind. The software is offered as-is. You are fully responsible for any use, misuse, instability, damage, or loss resulting from it.

## Legal / distribution note

- Green Curve is an unofficial third-party utility and is not affiliated with or endorsed by NVIDIA.
- It relies on driver interfaces exposed on systems with NVIDIA drivers already installed.
- NVIDIA names, product names, and trademarks remain the property of their respective owners.

## Release readiness notes

- Built for local Windows use on systems with an installed NVIDIA driver
- No network service, background daemon, or kernel component is shipped
- Hardware behavior can still vary by board vendor, VBIOS, cooling design, and driver version

## License

MIT, copyright (c) 2026 aufkrawall. See `LICENSE`.
