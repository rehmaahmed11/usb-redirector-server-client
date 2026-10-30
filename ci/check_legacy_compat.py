#!/usr/bin/env python3
"""Mini USB Bridge - legacy Windows (XP SP3 / Vista / 7 / 8 / 8.1) compatibility lint.

Two checks live here:

1. Source scan (always, no Windows needed)
   Every Windows API that does *not* exist on Windows XP SP3 must go through
   src/win/wincompat.cpp, which resolves it with GetProcAddress at runtime and
   falls back to the XP-era API. Any other use of such an API is an error, so a
   future change cannot silently raise the minimum Windows version.

2. PE header / import check (`--exe <file>`, optional)
   For a binary that claims to run on XP: the PE subsystem version must be
   5.01 and the import table must not reference post-XP functions or the UCRT
   (api-ms-win-crt-*). CRT DLLs from the XP toolset are allowed.

Usage:
    python3 ci/check_legacy_compat.py
    python3 ci/check_legacy_compat.py --exe dist/app/MiniUsbBridge.exe
    python3 ci/check_legacy_compat.py --quiet
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_DIRS = ["src", "relay"]

# The one file allowed to use post-XP APIs (it resolves them dynamically).
SHIM_FILE = os.path.normpath("src/win/wincompat.cpp")

# api : the Windows version that introduced it
DENY = {
    # Windows Vista
    "inet_pton": "Vista (ws2_32)",
    "inet_ntop": "Vista (ws2_32)",
    "InetPtonA": "Vista (ws2_32)",
    "InetPtonW": "Vista (ws2_32)",
    "InetNtopA": "Vista (ws2_32)",
    "InetNtopW": "Vista (ws2_32)",
    "BCryptGenRandom": "Vista (bcrypt.dll)",
    "BCryptOpenAlgorithmProvider": "Vista (bcrypt.dll)",
    "BCryptCloseAlgorithmProvider": "Vista (bcrypt.dll)",
    "BCryptDestroyKey": "Vista (bcrypt.dll)",
    "BCryptGenerateSymmetricKey": "Vista (bcrypt.dll)",
    "BCryptHashData": "Vista (bcrypt.dll)",
    "SHGetKnownFolderPath": "Vista (shell32)",
    "RegGetValueA": "Vista (advapi32)",
    "RegGetValueW": "Vista (advapi32)",
    "RegSetKeyValueW": "Vista (advapi32)",
    "CancelSynchronousIo": "Vista (kernel32)",
    "GetUserDefaultLocaleName": "Vista (kernel32)",
    "InitializeCriticalSectionEx": "Vista (kernel32)",
    "InitializeSRWLock": "Vista (kernel32)",
    "AcquireSRWLockExclusive": "Vista (kernel32)",
    "InitOnceExecuteOnce": "Vista (kernel32)",
    "CreateThreadpoolWork": "Vista (kernel32)",
    "GetFinalPathNameByHandleW": "Vista (kernel32)",
    "GetTickCount64": "Vista (kernel32)",
    "SetFileInformationByHandle": "Vista (kernel32)",
    "SetProcessDPIAware": "Vista (user32)",
    "GetSystemTimePreciseAsFileTime": "Windows 8 (kernel32)",
    "SetProcessDpiAwareness": "Windows 8.1 (shcore.dll)",
    "SetProcessDpiAwarenessContext": "Windows 10 1703 (user32)",
    "GetDpiForWindow": "Windows 10 1607 (user32)",
    "GetDpiForSystem": "Windows 10 1607 (user32)",
    "GetDpiForMonitor": "Windows 8.1 (shcore.dll)",
    "AdjustWindowRectExForDpi": "Windows 10 1607 (user32)",
    "EnableNonClientDpiScaling": "Windows 10 1607 (user32)",
    "SetThreadDpiAwarenessContext": "Windows 10 1607 (user32)",
    "SetThreadDescription": "Windows 10 1607 (kernel32)",
    "PathCchCanonicalizeEx": "Windows 8 (pathcch.dll)",
    "LoadIconWithScaleDown": "Vista (comctl32 v6)",
    "TaskDialogIndirect": "Vista (comctl32 v6)",
    "DwmSetWindowAttribute": "Vista (dwmapi.dll)",
    "DwmExtendFrameIntoClientArea": "Vista (dwmapi.dll)",
    "SetWindowCompositionAttribute": "Windows 10 (user32, undocumented)",
    "GetSystemMetricsForDpi": "Windows 10 1607 (user32)",
    "SetProcessMitigationPolicy": "Windows 8 (kernel32)",
    "GetPackageFamilyName": "Windows 8 (kernel32)",
}

# Libraries that must not be linked statically for an XP target.
DENY_LIBS = {
    "bcrypt.lib": "Vista (bcrypt.dll is loaded dynamically instead)",
    "shcore.lib": "Windows 8.1",
    "dwmapi.lib": "Vista",
    "pathcch.lib": "Windows 8",
}

# A call to one of these is allowed but should be reviewed: the wrapper exists,
# yet a new use means a new behaviour on old Windows.
REVIEW = {"GetVersionExW": "lies on Windows 8.1+; only wincompat may call it"}

CRT_ALLOWED_DLLS = ("msvcrt.dll", "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll",
                    "shell32.dll", "ole32.dll", "oleaut32.dll", "comctl32.dll", "setupapi.dll",
                    "cfgmgr32.dll", "ws2_32.dll", "secur32.dll", "crypt32.dll", "uuid.dll",
                    "ntdll.dll", "rpcrt4.dll", "shlwapi.dll", "version.dll", "winmm.dll",
                    "imm32.dll", "comdlg32.dll", "wininet.dll")

CRT_RUNTIME_DLLS = ("vcruntime140", "vcruntime140_1", "msvcp140", "msvcp_win", "vcruntime", "ucrtbase")


def iter_sources(dirs=None):
    for base in (dirs or SOURCE_DIRS):
        for dirpath, _dirnames, filenames in os.walk(os.path.join(ROOT, base)):
            for name in sorted(filenames):
                if name.endswith((".cpp", ".h", ".c")):
                    yield os.path.join(dirpath, name)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def scan_lines(text: str):
    """Yields (line_number, code_without_comments, raw_line)."""
    in_block = False
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw
        out = []
        i = 0
        while i < len(line):
            if in_block:
                end = line.find("*/", i)
                if end < 0:
                    i = len(line)
                    break
                in_block = False
                i = end + 2
                continue
            start = line.find("/*", i)
            slashes = line.find("//", i)
            if slashes >= 0 and (start < 0 or slashes < start):
                out.append(line[i:slashes])
                i = len(line)
                break
            if start < 0:
                out.append(line[i:])
                i = len(line)
                break
            out.append(line[i:start])
            in_block = True
            i = start + 2
        yield number, "".join(out), raw


def check_sources(dirs=None, quiet=False) -> int:
    problems = []
    scanned = 0
    for path in iter_sources(dirs):
        rel = os.path.normpath(os.path.relpath(path, ROOT))
        scanned += 1
        if rel == SHIM_FILE:
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = list(scan_lines(fh.read()))
        # A use may be exempted with a "xp-ok: <reason>" comment on the line or
        # within the three lines above it (used for #if _WIN32_WINNT guards).
        raw_lines = [raw for _n, _code, raw in lines]
        for index, (number, code, _raw) in enumerate(lines):
            if not code.strip():
                continue
            window = " ".join(raw_lines[max(0, index - 3):index + 1])
            exempt = "xp-ok" in window
            for api, since in DENY.items():
                if re.search(r"(?<![A-Za-z0-9_])" + re.escape(api) + r"\s*\(", code):
                    if not exempt:
                        problems.append(f"{rel}:{number}: uses {api}() - {since} - move it into {SHIM_FILE}")
            for lib, since in DENY_LIBS.items():
                if re.search(r'pragma\s+comment\s*\(\s*lib\s*,\s*"' + re.escape(lib), code):
                    if not exempt:
                        problems.append(f"{rel}:{number}: links {lib} statically - {since}")
            for api, why in REVIEW.items():
                if re.search(r"(?<![A-Za-z0-9_])" + re.escape(api) + r"\s*\(", code):
                    if not exempt:
                        problems.append(f"{rel}:{number}: calls {api}() directly - {why}")

    if not quiet:
        print(f"source scan: {scanned} files, {len(DENY)} post-XP APIs, {len(DENY_LIBS)} forbidden libraries")
    if problems:
        if not quiet:
            print("\nLEGACY COMPATIBILITY VIOLATIONS")
        for p in problems:
            print("  FAIL " + p)
        return 1
    if not quiet:
        print("  ok   no post-XP API is used outside the runtime shim")
    return 0


def self_test() -> int:
    """Proves the lint actually catches violations (a check that never fails is useless)."""
    import tempfile

    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        bad = os.path.join(tmp, "bad.cpp")
        with open(bad, "w", encoding="utf-8") as fh:
            fh.write('#include <winsock2.h>\nvoid f(){ char b[8]; inet_pton(AF_INET, "1.1.1.1", b); }\n')
        good = os.path.join(tmp, "good.cpp")
        with open(good, "w", encoding="utf-8") as fh:
            fh.write('void f(){ MubInetPton(AF_INET, "1.1.1.1", b); }\n'
                     '// xp-ok: guarded by _WIN32_WINNT\nint g(){ return GetTickCount64(); }\n')
        rel_dir = os.path.relpath(tmp, ROOT)
        code = check_sources(dirs=[rel_dir], quiet=True)
        print("self test: violation detected" if code == 1 else "self test: FAIL - violation was not detected")
        failures += 0 if code == 1 else 1

        os.remove(bad)
        code = check_sources(dirs=[rel_dir], quiet=True)
        print("self test: exempt use and shim call accepted" if code == 0 else "self test: FAIL - false positive")
        failures += 0 if code == 0 else 1

    return 1 if failures else 0


# ---------------------------------------------------------------------------
# PE import check
# ---------------------------------------------------------------------------
def pe_imports(path: str):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    coff = pe + 4
    nsec, = struct.unpack_from("<H", data, coff + 2)
    optsize, = struct.unpack_from("<H", data, coff + 16)
    opt = coff + 20
    magic, = struct.unpack_from("<H", data, opt)
    pe32p = magic == 0x20B
    ddoff = opt + (112 if pe32p else 96)
    subsys_major, subsys_minor = struct.unpack_from("<HH", data, opt + 48)

    sections = []
    secbase = opt + optsize
    for i in range(nsec):
        off = secbase + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((vaddr, max(vsize, rawsize), rawptr))

    def rva_to_off(rva):
        for vaddr, size, rawptr in sections:
            if vaddr <= rva < vaddr + size:
                return rawptr + (rva - vaddr)
        return None

    imports = {}

    def collect(directory_index):
        rva, = struct.unpack_from("<I", data, ddoff + directory_index * 8)
        if not rva:
            return
        off = rva_to_off(rva)
        if off is None:
            return
        while True:
            ilt, _ts, _fc, name_rva, iat = struct.unpack_from("<IIIII", data, off)
            if not ilt and not name_rva:
                break
            name_off = rva_to_off(name_rva)
            if name_off:
                end = data.index(b"\0", name_off)
                dll = data[name_off:end].decode("ascii", "replace").lower()
                names = set()
                table = rva_to_off(ilt or iat)
                if table is not None:
                    i = 0
                    while True:
                        entry, = struct.unpack_from("<I", data, table + i * 4)
                        if entry == 0:
                            break
                        if not entry & 0x80000000:
                            hint_off = rva_to_off(entry & 0x7FFFFFFF)
                            if hint_off:
                                fend = data.index(b"\0", hint_off + 2)
                                names.add(data[hint_off + 2:fend].decode("ascii", "replace"))
                        i += 1
                imports.setdefault(dll, set()).update(names)
            off += 20

    collect(1)   # Import directory
    collect(13)  # Delay-load import directory
    return imports, (subsys_major, subsys_minor)


def check_exe(path: str, target: str = "xp") -> int:
    """Verifies a built binary.

    target=xp    : PE subsystem 5.01, no UCRT/vcruntime140, no post-XP imports.
    target=win10 : only the "our own code must resolve newer APIs dynamically"
                   rule; the Windows 10 CRT imports are expected and allowed.
    """
    if not os.path.exists(path):
        print(f"  skip {path} does not exist")
        return 0
    imports, subsystem = pe_imports(path)
    problems = []
    print(f"PE check ({target} rules): {path} ({os.path.getsize(path)} bytes)")
    if target == "xp":
        if subsystem != (5, 1):
            problems.append(f"subsystem version is {subsystem[0]}.{subsystem[1]:02d}; Windows XP needs 5.01 "
                            f"(link with /SUBSYSTEM:...,5.01)")
        else:
            print("  ok   subsystem version 5.01")
        for dll in sorted(imports):
            if dll.startswith("api-ms-win-crt-"):
                problems.append(f"imports the UCRT ({dll}) - Windows 10 only; use the static CRT")
            elif dll.startswith(CRT_RUNTIME_DLLS):
                problems.append(f"imports {dll} - the XP toolset's static runtime (msvcrt.dll) is required")
            elif dll.startswith("ext-ms-"):
                problems.append(f"imports {dll} - Windows 8+ API set")
            elif dll not in CRT_ALLOWED_DLLS and not dll.startswith(CRT_RUNTIME_DLLS):
                problems.append(f"imports {dll} which does not exist on Windows XP")

    # Both targets: our own code must never call a post-XP API directly - the
    # runtime shim resolves it with GetProcAddress instead.
    for dll, names in imports.items():
        is_crt = dll.startswith(CRT_RUNTIME_DLLS) or dll.startswith("api-ms-win-crt-") or dll == "msvcrt.dll"
        if is_crt:
            continue
        for name in sorted(names):
            if name in DENY:
                problems.append(f"imports {name} from {dll} - {DENY[name]}")

    if problems:
        print("\nPE COMPATIBILITY VIOLATIONS")
        for p in problems:
            print("  FAIL " + p)
        return 1
    total = sum(len(v) for v in imports.values())
    print(f"  ok   {total} imported functions, {len(imports)} DLLs, none post-XP in our code")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", action="append", default=[], help="also verify a built binary")
    ap.add_argument("--target", choices=["xp", "win10"], default="xp",
                    help="rules for --exe (default: xp)")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--self-test", action="store_true", help="verify that the lint detects violations")
    args = ap.parse_args()

    rc = self_test() if args.self_test else check_sources()
    for exe in args.exe:
        rc |= check_exe(exe, args.target)
    if rc == 0 and not args.quiet:
        print("\nlegacy compatibility: PASS")
    elif rc != 0:
        print("\nlegacy compatibility: FAIL")
    return rc


if __name__ == "__main__":
    sys.exit(main())
