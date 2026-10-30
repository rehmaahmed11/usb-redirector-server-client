# Bundled backend installers (offline installer)

This directory is populated **automatically at build time** so the setup is fully
offline - no extra downloads at install time.

* `usbipd-win_*.msi`  - from https://github.com/dorssel/usbipd-win/releases (GPL-3.0, server side)
* `USBip-*.exe` / `usbip-win2-*.exe` - from https://github.com/vadimgrn/usbip-win2/releases (BSD-2-Clause, client side)

When you build the installer:

```powershell
# Windows (CI or local)
pwsh -File ci/fetch-backends.ps1
iscc installer/MiniUsbBridge.iss

# Linux / any host (also works in CI)
python3 ci/fetch-backends.py
python3 ci/check_iss_sources.py installer/MiniUsbBridge.iss --allow 'dist/app/*'
```

The fetch scripts query the GitHub API for the latest releases, download the
official **unmodified** vendor installers, verify size/SHA-256 and write
`BUNDLED.md` + `versions.json` with provenance (version, URL, license, hash).

The Inno Setup script embeds everything it finds here:

```iss
Source: "backends\usbipd-win*.msi"; DestDir: "{tmp}\mub-backends"; ...
Source: "backends\usbip-win2*.exe"; DestDir: "{tmp}\mub-backends"; ...
Source: "backends\USBip*.exe";     DestDir: "{tmp}\mub-backends"; ...
```

At install time the setup:

1. Detects whether a backend is already installed (`usbipd.exe` in PATH / `Program Files`)
2. If the user chose **Server** (or **Both**) and `usbipd-win` is missing, it installs
   the bundled MSI silently via `msiexec /i ... /qn /norestart`
3. If the user chose **Client** (or **Both**) and `usbip-win2` is missing, it installs
   the bundled EXE via `/VERYSILENT /SUPPRESSMSGBOXES`
4. Already-installed backends are never overwritten; the bundled copy is deleted from
   `%TEMP%\mub-backends` after installation (`deleteafterinstall`)

Manual override: drop the official installers here yourself before running `iscc`.
They are intentionally **not** stored in git (size + license hygiene) but they
**are** included in the distributed `MiniUsbBridge-*-setup.exe` that CI builds.
The installed app keeps a copy of `BUNDLED.md` in `{app}\docs\` so a user can
see exactly which vendor versions were bundled.

### License compliance

- **usbipd-win** is GPL-3.0. Source: https://github.com/dorssel/usbipd-win -
  the binary is redistributed under GPL-3.0; the vendor repository is the
  source offer.
- **usbip-win2** is BSD-2-Clause: https://github.com/vadimgrn/usbip-win2/blob/master/LICENSE
- This app (Mini USB Bridge) is MIT. It does not link to backend code; it only
  runs their installers and invokes their CLI tools, so the licenses do not
  mix.
