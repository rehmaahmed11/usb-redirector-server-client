# CI

Two jobs, one goal: prove that every merge still produces a working
`MiniUsbBridge.exe`, `mubb-relay.exe` and a compiling Inno Setup installer — and
leave behind a report that explains what happened, even when it failed.

```
push to main / pull request / manual
        |
        +-- core (ubuntu-latest)     ci/linux-check.sh
        |     g++ core tests -> test_core -> g++ relay -> relay protocol tests
        |     -> check_iss_sources.py -> docs link check
        |
        +-- windows (windows-latest)  ci/windows-build.ps1
              cmake configure -> MSVC build -> test_core.exe -> dist\app
              -> check_iss_sources.py -> ISCC.exe -> verify + SHA-256
```

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
| `ci/publish-report.ps1` | posts the report to the PR / a check run; never fails the job |

## Downloading a build

Actions → the run → **Artifacts**:

* `MiniUsbBridge-windows-x64` — the exe, the relay, `MiniUsbBridge-1.0.0-setup.exe` and `SHA256SUMS.txt`;
* `toolchain-report-windows` / `toolchain-report-linux` — reports and per-stage logs.

The binaries are **unsigned**, so SmartScreen warns on first run. Signing needs a
code-signing certificate, which CI cannot invent; see `docs/TESTING.md`.
