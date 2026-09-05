# Arch Linux Packaging for Green Curve

This directory contains the packaging specifications for Arch Linux and the Arch User Repository (AUR).

## Files

| File | Purpose |
| --- | --- |
| `PKGBUILD` | Builds and packages Green Curve from source (`greencurve`) |
| `PKGBUILD.bin` | Packages official precompiled release tarballs (`greencurve-bin`) |
| `greencurve.service` | Hardened systemd daemon service unit (`/usr/bin/greencurve --daemon`) |
| `greencurve-resume.service` | Systemd standby-resume restore unit (`/usr/bin/greencurve --resume-restore`) |
| `greencurve.sysusers` | Declarative `g greencurve -` group creation for `systemd-sysusers` |
| `greencurve.desktop` | Desktop application launcher (`greencurve --tui --from-desktop`) |
| `greencurve.install` | Post-install/upgrade scriptlet for user guidance |

## Building and Installing

### Building from source with makepkg

Ensure build prerequisites are installed:

```bash
sudo pacman -S --needed base-devel python zig
```

Clone or copy the files and run `makepkg`:

```bash
cd packaging/arch
makepkg -si
```

### Precompiled release package (`greencurve-bin`)

To build the package from precompiled official release tarballs:

```bash
cd packaging/arch
makepkg -p PKGBUILD.bin -si
```

## Post-Installation Steps

1. Add your user account to the `greencurve` group to grant unprivileged GPU control access:
   ```bash
   sudo usermod -aG greencurve $USER
   ```
   Sign out and back in (or run `newgrp greencurve` in your current terminal).

2. Enable and start the background daemon:
   ```bash
   sudo systemctl enable --now greencurve.service
   ```

3. Launch the terminal UI from any terminal:
   ```bash
   greencurve
   ```
