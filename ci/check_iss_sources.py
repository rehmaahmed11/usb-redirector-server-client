#!/usr/bin/env python3
"""Static check of the Inno Setup script before Inno Setup runs.

Inno Setup fails with a terse "Unable to locate file" at the end of compilation,
after the whole pipeline has already run. This checks the same thing in a second,
so a missing doc or a renamed binary is caught early on any OS (it is also run on
the Linux job, where ISCC does not exist at all).

Checks:
  * every [Files] Source: path resolves to at least one file (wildcards allowed),
    unless the entry has skipifsourcedoesntexist
  * LicenseFile= and SetupIconFile= exist
  * OutputDir is writable-ish (just reported)

Usage:  python3 ci/check_iss_sources.py installer/MiniUsbBridge.iss [--allow GLOB]...
Exit:   0 = all good, 1 = problems found.

--allow marks a path pattern as "produced later by the build" instead of an error,
so the check is usable before the binaries exist (Linux job) and strict after they
do (Windows job runs it right before ISCC, with no --allow flags).

Preprocessor defines (``#define AppExe "MiniUsbBridge.exe"``) and ``{#AppExe}``
references are resolved, because that is how the script spells its own binary.
SPDX-License-Identifier: MIT
"""
import fnmatch
import glob
import os
import re
import sys

SOURCE_RE = re.compile(r'^\s*Source:\s*"([^"]+)"\s*(.*)$', re.IGNORECASE)
KEY_RE = re.compile(r'^\s*(LicenseFile|SetupIconFile|OutputDir)\s*=\s*(.+?)\s*$', re.IGNORECASE)
DEFINE_RE = re.compile(r'^\s*#define\s+(\w+)\s+"([^"]*)"', re.IGNORECASE)
REF_RE = re.compile(r'\{#(\w+)\}')


def expand(text, defines):
    return REF_RE.sub(lambda m: defines.get(m.group(1), m.group(0)), text)


def main(argv):
    args = [a for a in argv[1:]]
    allowed = []
    positional = []
    i = 0
    while i < len(args):
        if args[i] == "--allow" and i + 1 < len(args):
            allowed.append(os.path.normpath(args[i + 1]))
            i += 2
            continue
        positional.append(args[i])
        i += 1
    if len(positional) != 1:
        print("usage: check_iss_sources.py <installer.iss> [--allow GLOB]...")
        return 2

    iss_path = positional[0]
    if not os.path.isfile(iss_path):
        print(f"FAIL  installer script not found: {iss_path}")
        return 1

    defines = {}
    with open(iss_path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            m = DEFINE_RE.match(raw)
            if m:
                defines[m.group(1)] = m.group(2)
    if defines:
        print("defines: " + ", ".join(f"{k}={v}" for k, v in sorted(defines.items())))

    base = os.path.dirname(os.path.abspath(iss_path))
    # --allow patterns are given relative to the repository root (the .iss lives
    # one level down, in installer/).
    repo_root = os.path.dirname(base)
    allow_abs = [os.path.normpath(os.path.join(repo_root, a)) for a in allowed]
    section = ""
    problems = []
    checked = []
    skipped = []

    with open(iss_path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.strip()
            if line.startswith("[") and line.endswith("]"):
                section = line
                continue
            if not line or line.startswith(";") or line.startswith("//"):
                continue

            if section.lower() == "[files]":
                m = SOURCE_RE.match(line)
                if not m:
                    continue
                rel = expand(m.group(1), defines).replace("\\", os.sep)
                params = m.group(2).lower()
                pattern = os.path.normpath(os.path.join(base, rel))
                matches = glob.glob(pattern)
                if matches:
                    checked.append((lineno, rel, len(matches)))
                elif "skipifsourcedoesntexist" in params:
                    skipped.append((lineno, rel))
                elif any(fnmatch.fnmatch(pattern, a) for a in allow_abs):
                    skipped.append((lineno, rel + " (produced by the build)"))
                else:
                    problems.append(f"line {lineno}: [Files] source not found: {rel}")

            m2 = KEY_RE.match(line)
            if m2:
                key, value = m2.group(1), m2.group(2)
                if key.lower() == "outputdir":
                    print(f"note  OutputDir = {value} (relative to the .iss file)")
                    continue
                rel = expand(value, defines).replace("\\", os.sep)
                path = os.path.normpath(os.path.join(base, rel))
                if os.path.exists(path):
                    checked.append((lineno, rel, 1))
                else:
                    problems.append(f"line {lineno}: {key} not found: {rel}")

    print(f"checked {iss_path}")
    for lineno, rel, count in checked:
        suffix = f" ({count} file(s) via wildcard)" if count > 1 else ""
        print(f"  ok    line {lineno:>4}: {rel}{suffix}")
    for lineno, rel in skipped:
        if "produced by the build" in rel:
            print(f"  later line {lineno:>4}: {rel}")
        else:
            print(f"  skip  line {lineno:>4}: {rel} (skipifsourcedoesntexist)")

    if problems:
        print("")
        for p in problems:
            print(f"  FAIL  {p}")
        print(f"\n{len(problems)} problem(s) - Inno Setup would fail on these.")
        return 1

    print("\nall installer sources present")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
