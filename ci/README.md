# CI

Three jobs, one goal: prove that every merge still produces a working
`MiniUsbBridge.exe`, `mubb-relay.exe` and a compiling Inno Setup installer — and
leave behind a report that explains what happened, even when it failed.

```
push to main / pull request / manual
        |
        +-- core (ubuntu-latest)     ci/linux-check.sh
        |     g++ core tests -> test_core -> g++ relay -> relay protocol tests
        |     -> check_legacy_compat.py (+ --self-test)  [XP/7/8/10/11 API lint]
        |     -> xp-compile-check.sh (_WIN32_WINNT=0x0501 build, MinGW-w64 i686)
        |     -> check_iss_sources.py x3 -> fetch-backends dry-run -> compat docs -> docs link check
        |
        +-- windows-legacy (windows-latest)  .\build.ps1 -MinWin vista -Installer
        |     _WIN32_WINNT=0x0600 + static CRT -> dist\app-legacy
        |     -> check_legacy_compat.py --exe (PE imports) -> MiniUsbBridgeLegacy.iss
        |
        +-- windows (windows-latest)  ci/windows-build.ps1
              cmake configure -> MSVC build -> test_core.exe -> dist\app
              -> fetch backends (usbipd-win + usbip-win2, offline bundle)
              -> check_iss_sources.py -> ISCC.exe -> setup.exe (fully offline)
              -> verify + SHA-256
```

## Compatibility checks (Windows XP ... Windows 11)

Three checks make sure a merge cannot silently raise the minimum Windows version;
all of them are in the Linux job, so they run in seconds and need no Windows:

| Check | What it proves |
|---|---|
| `ci/check_legacy_compat.py` | no post-XP API is called outside `src/win/wincompat.cpp` (46 APIs, 4 static libraries), plus `--self-test` which proves the lint fails on a violation |
| `ci/xp-compile-check.sh` | every source compiles with `-D_WIN32_WINNT=0x0501` (MinGW-w64 i686), where the headers hide post-XP declarations; skips cleanly without a cross compiler |
| `ci/check_legacy_compat.py --exe ... --target xp` | a built XP binary uses PE subsystem 5.01, no UCRT import, no post-XP import (used by `build.ps1 -MinWin xp`, informational for the Vista target) |

`docs/COMPATIBILITY.md` is the human-readable version of the same rules, and the
per-OS verdicts come from `src/core/os_support.cpp`, which is unit tested (362
checks in `tools/test_core.sh`).

## The tool chain report

Every job writes `dist/ci/toolchain-report*.md` with:

* the trigger, ref, commit, runner image and timestamp;
* **one row per stage** — command, result, duration, log file name;
* **every diagnostic line** from the failing log (`error C…`, `error LNK…`,
  `CMake Error`, …) plus the last 40 lines, when a stage fails;
* the tool versions actually used (`cl.exe`, MSVC toolset, Windows SDK,
  CMake, Inno Setup, python);
* an artifact inventory with size, PE machine, file version, signature status
  and SHA-256;
* an explicit "what this run does not prove" section.

The Windows script writes the report from a `finally` block, so it exists even
when a stage throws. It is published three ways:

1. as an artifact (`toolchain-report-windows`, `toolchain-report-linux`,
   `MiniUsbBridge-windows-x64`), with the per-stage logs attached;
2. appended to the **run summary** in the Actions UI;
3. posted by `ci/publish-report.ps1` as an **upserted PR comment** (or, for a
   merge to `main`, as a check run whose output text is the report) — so the
   report is readable without the log viewer.

## Scripts

| File | Purpose |
|---|---|
| `ci/windows-build.ps1` | the whole Windows pipeline, timed and self-reporting; also usable locally: `pwsh -File ci\windows-build.ps1` |
| `ci/linux-check.sh` | portable core + relay + installer-source checks on any POSIX host |
| `ci/check_iss_sources.py` | pre-flight of the Inno Setup script: every `[Files]`/`LicenseFile`/`SetupIconFile` source must exist (resolves `#define` references, honours `skipifsourcedoesntexist`, `--allow` for not-yet-built binaries) |
| `ci/fetch-backends.py` / `ps1` | downloads the official backend installers into `installer/backends/` so the setup is fully offline (run automatically in CI, or manually: `python3 ci/fetch-backends.py`) |
| `ci/publish-report.ps1` | posts the report to the PR / a check run; never fails the job |

## Downloading a build

Actions → the run → **Artifacts**:

* `MiniUsbBridge-windows-x64` — the exe, the relay, **fully offline** `MiniUsbBridge-1.0.0-setup.exe` (backends already inside) and `SHA256SUMS.txt`;
* `toolchain-report-windows` / `toolchain-report-linux` — reports and per-stage logs.

The binaries are **unsigned**, so SmartScreen warns on first run. Signing needs a
code-signing certificate, which CI cannot invent; see `docs/TESTING.md`.
