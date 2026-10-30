# Compatibility: Windows XP SP3 → Windows 11

This page answers one question honestly: **what works on which Windows version?**

There are three separate things, and they have three different minimums:

| Piece | Who makes it | Minimum Windows |
|---|---|---|
| **Mini USB Bridge itself** — window, pairing, approval, TLS tunnel, relay client | this project (MIT) | **Windows XP SP3** (x86 or x64) |
| **SERVER backend** — exports the local USB device (`usbipd.exe`, service on :3240) | [usbipd-win](https://github.com/dorssel/usbipd-win) (GPL-3.0) | **Windows 10 1809** (build 17763) **x64/ARM64**, Windows Server 2019+ |
| **CLIENT backend** — the virtual USB host controller that makes the remote device appear in Device Manager | [usbip-win2](https://github.com/vadimgrn/usbip-win2) (BSD-2-Clause) | **Windows 10 1903** (build 18362) **x64**, Windows 11 ARM64 |

The two backend minimums are the vendors' own requirements, not something this
project can lower: both are kernel drivers / services built against Windows 10
kernel interfaces, and neither ships an x86 build. The app cannot invent a driver.

## The matrix

Generated from the same code the app runs (`mub::CompatibilityMatrixMarkdown()`
in `src/core/os_support.cpp`), so the table cannot drift from the behaviour:

| Windows version | App (GUI + tunnel) | Server backend (usbipd-win) | Client backend (usbip-win2) | TLS used |
|---|---|---|---|---|
| Windows XP SP3 (x86) | yes | **no** | **no** | TLS 1.0 |
| Windows XP x64 Edition / Server 2003 | yes | **no** | **no** | TLS 1.0 |
| Windows Vista SP2 | yes | **no** | **no** | TLS 1.2 (1.0-1.2 offered) |
| Windows 7 SP1 | yes | **no** | **no** | TLS 1.2 (1.0-1.2 offered) |
| Windows 8 | yes | **no** | **no** | TLS 1.2 (1.0-1.2 offered) |
| Windows 8.1 | yes | **no** | **no** | TLS 1.2 (1.0-1.2 offered) |
| Windows 10 1507 - 1803 | yes | **no** | **no** | TLS 1.2 |
| Windows 10 1809 | yes | yes | **no** | TLS 1.2 |
| Windows 10 1903 - 1909 | yes | yes | yes | TLS 1.2 |
| Windows 10 2004 - 22H2 | yes | yes | yes | TLS 1.2 |
| Windows 10 (32-bit x86) | yes | **no** | **no** | TLS 1.2 |
| Windows 11 21H2 - 25H2 | yes | yes | yes | TLS 1.2 |
| Windows 11 ARM64 | yes | yes | yes | TLS 1.2 |
| Windows Server 2019 | yes | yes | **no** | TLS 1.2 |
| Windows Server 2022 | yes | yes | yes | TLS 1.2 |

"App: yes" means the window starts, pairing codes are exchanged, the encrypted
tunnel and the relay work, devices are enumerated and the backend state is
reported. It does **not** mean USB redirection works on that OS — that is the
backend column, and the app says the same thing in its own banner and log:

```
os  Detected Windows 7, build 6.1.7601, x64
os  TLS: TLS 1.0-1.2 (TLS 1.2 preferred, TLS 1.0 kept for XP peers)
os  Server backend (usbipd-win): NOT supported - usbipd-win needs Windows 10 1809
    or later (x64/ARM64); this PC runs Windows 7, build 6.1.7601, x64 - SERVER
    mode cannot export USB devices here
os  Client backend (usbip-win2): NOT supported - usbip-win2 needs Windows 10 1903
    or later (x64) or Windows 11 ARM64; ...
os  Limitation: App, pairing and encrypted tunnel run on Windows 7 ... but USB
    redirection needs a backend that this Windows version cannot run ...
```

## What you can still do on Windows XP / Vista / 7 / 8 / 8.1

* Run the app on **both** PCs and use it as the **encrypted control/tunnel layer**
  for an older backend that you install and support yourself.
* Use the app on an old PC as the **CLIENT** when a *legacy* USB/IP client driver
  is already installed. The app detects the legacy WDM VHCI driver
  (`%SystemRoot%\System32\drivers\usbip_vhci.sys`, from
  [cezanne/usbip-win](https://github.com/cezanne/usbip-win) v0.1.0) and then:
  * prints *"legacy WDM VHCI driver (test-signed, unsupported)"* in the backend
    line,
  * moves the loopback tunnel to **3240** automatically, because old `usbip.exe`
    builds have no `--tcp-port` switch,
  * retries `attach` without the global option when the binary rejects it,
  * keeps the picture honest: no driver, no attach — it never reports success
    that Device Manager does not show.
* Use the app on an old PC as the **SERVER** only if you install a legacy
  `usbipd` yourself; usbipd-win cannot be installed there and the app says so.

The legacy driver path is **not bundled, not signed for modern Windows, and not
tested by this project**. It exists because users asked for XP/7 support; a
kernel driver from 2020 with test signing is a security decision you make
yourself (`bcdedit /set TESTSIGNING ON` + reboot).

## TLS on old Windows

| OS | Protocols SChannel offers | Notes |
|---|---|---|
| Windows XP / Server 2003 (5.x) | TLS 1.0 only | No TLS 1.1/1.2 was ever shipped publicly for XP. The strong-crypto flag is **not** set on XP so 3DES/AES suites remain available (AES needs XP SP3). A server identity created on XP is signed with **SHA-1**, because XP's SChannel cannot verify SHA-256 signatures. |
| Windows Vista / 7 / 8 / 8.1 | TLS 1.0, 1.1, 1.2 | TLS 1.2 is preferred; 1.0 stays enabled so an XP PC can still connect. |
| Windows 10 / 11 | OS policy (TLS 1.2 negotiated) | The app uses the `SCHANNEL_CRED` structure, which cannot request TLS 1.3 (Microsoft requires the newer `SCH_CREDENTIALS` for that), so the handshake settles on TLS 1.2. |
| Unknown / future | TLS 1.2 | Conservative default. |

The certificate **pin** (10 bytes of SHA-256 over the DER certificate) still
protects the channel on every one of these versions: the client does not rely on
the OS trust chain, so an old SChannel cannot be talked into trusting an
attacker's certificate.

**Whose policy wins?** On Windows XP the app has to name the protocol explicitly
(`SP_PROT_TLS1_0_CLIENT/SERVER`), because the OS default is not usable across XP
patch levels. From Vista on the app passes *no* protocol mask when legacy TLS is
allowed, so an administrator who disabled TLS 1.0/1.1 through the registry keeps
that decision — SChannel still negotiates TLS 1.2 with a modern peer and TLS 1.0
only with an XP/Vista peer that cannot do better. The negotiated protocol is
logged (`tls ... negotiated TLS 1.2 / TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384`).

**Turning legacy TLS off.** Settings → *Accept TLS 1.0/1.1 for old Windows peers*
(`allowLegacyTls=0` in `settings.ini`) pins the protocol set to TLS 1.2 exactly.
Everything from Windows 7 up then still connects, and an XP/Vista peer is refused
instead of silently downgrading the whole session.

## How the app stays XP-compatible

Windows XP is missing APIs that a modern compiler happily declares, so every one
of them goes through `src/win/wincompat.cpp`, resolved at runtime with
`GetProcAddress` and with an XP-era fallback:

| Need | Modern API | XP fallback in `wincompat.cpp` |
|---|---|---|
| OS detection | `RtlGetVersion` (ntdll, truthful) | `GetVersionExW` |
| Random bytes | `BCryptGenRandom` (bcrypt.dll, Vista+) | `CryptGenRandom` (advapi32) |
| Address parsing | `inet_pton` / `inet_ntop` (Vista+) | `inet_addr` / `WSAStringToAddressA` / `inet_ntoa` |
| DPI | `GetDpiForWindow`, `SetProcessDpiAwarenessContext/SetProcessDpiAwareness` | `GetDeviceCaps(LOGPIXELSX)`, `SetProcessDPIAware`, nothing |
| Architecture / WOW64 | `GetNativeSystemInfo`, `IsWow64Process` | same calls, both XP-safe (resolved dynamically) |
| Thread-safe one-shot init | — | `std::mutex` gate instead of `std::call_once`, because `std::call_once` imports `InitOnceExecuteOnce` (Vista+) and would make the XP binary unloadable |

Three checks keep it that way; all of them run in CI on every pull request:

1. `ci/check_legacy_compat.py` — textual lint over `src/` and `relay/`:
   46 post-XP APIs and 4 forbidden static libraries are rejected outside
   `wincompat.cpp`. `--self-test` proves the lint really fails on a violation.
2. `ci/xp-compile-check.sh` — compiles **every** Windows source with
   `-D_WIN32_WINNT=0x0501`, where the headers hide post-XP functions entirely.
3. `python3 ci/check_legacy_compat.py --exe <binary>` — optional PE check for an
   XP artifact: PE subsystem version must be `5.01`, no `api-ms-win-crt-*`
   (UCRT = Windows 10) imports, no post-XP function imports.

The manifest (`src/ui/app.manifest`) declares every supported OS and
`processorArchitecture="*"`, so the same manifest is valid for x86, x64 and ARM64.

## Building per target

| Target | Command | Toolchain |
|---|---|---|
| Windows 10/11 (default) | `.\build.ps1` | Visual Studio 2019/2022, Windows 10 SDK |
| Vista / 7 / 8 / 8.1 | `.\build.ps1 -MinWin vista` | any of the above (`/MT`, `_WIN32_WINNT=0x0600`) |
| Windows XP SP3 / Server 2003 | `.\build.ps1 -MinWin xp` | **Visual Studio 2017 or 2019** + *"C++ Windows XP Support for VS 2017 (v141) tools"* component (toolset `v141_xp`) |
| any target, cross compile | `cmake -S . -B build -DMUB_MIN_WIN=xp` | MinGW-w64 i686 (`-D_WIN32_WINNT=0x0501`) |

`MUB_MIN_WIN=xp` forces the static CRT (the XP toolset does not support a DLL
runtime) and `/SUBSYSTEM:WINDOWS,5.01`; without that subsystem version, XP
refuses to start the program at all ("not a valid Win32 application").

Visual Studio 2022 does not ship the XP toolset. That is why XP builds are a
documented, manual step: build them on a 2017/2019 machine (or with MinGW-w64)
and keep the result next to the normal build.

## Installers

| Installer | Minimum Windows | Bundled backends |
|---|---|---|
| `installer/MiniUsbBridge.iss` (default) | Windows 10 1809 (`MinVersion=10.0.17763`), x64/ARM64 | yes, offline |
| `installer/MiniUsbBridgeLegacy.iss` | Windows 7 SP1 (`MinVersion=6.1sp1`), x86 + x64/ARM64 | no (links instead) |
| `installer/MiniUsbBridgeXp.iss` | Windows XP SP3 (`MinVersion=5.1`), x86 + x64 | no |

The XP script needs **Inno Setup 5.6.1** (`ISCC.exe` from a 5.6.x install);
Inno Setup 6 refuses any `MinVersion` below 6.0 by design. Both legacy scripts
copy the app, the docs and (optionally) the relay, and they explicitly do **not**
try to install a driver on an OS that cannot load it.

## Not claimed

* No automation has run these binaries on real XP/Vista/7/8/8.1 hardware. What is
  proven in CI is the API surface (compile + lint) and the logic (unit tests).
  `docs/TESTING.md` has the row-by-row manual checklist to close that gap.
* Windows 10/11 ARM64 support is only as good as the vendor backends' ARM64
  builds; the app itself compiles for ARM64 but has not been run on such a device.
* The legacy VHCI driver path is best effort. If Device Manager shows no new USB
  device, the app says the attach is not trusted — by design.
