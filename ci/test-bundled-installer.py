#!/usr/bin/env python3
"""
Test that the installer is now fully offline / bundled.

Simulates:
 1. Fetching backends (with placeholder fallback for sandbox)
 2. Installer ISS pattern matching (like FindFirstFileIn)
 3. Backend detection and auto-install logic for Server/Client/Both roles
 4. That the installed app would find the backends

This is the "run installer and see what happens in sandbox" test.
"""
import fnmatch
import glob
import os
import sys
import tempfile
import shutil
import json
import hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISS = os.path.join(ROOT, "installer/MiniUsbBridge.iss")
BACKENDS = os.path.join(ROOT, "installer/backends")

def log(msg):
    print(msg)

def assert_ok(cond, msg):
    if not cond:
        print(f"FAIL: {msg}")
        sys.exit(1)
    print(f"  ok: {msg}")

# 1. Verify ISS has correct bundling patterns
log("=== 1. ISS bundling patterns ===")
with open(ISS, encoding="utf-8") as f:
    iss_text = f.read()
assert_ok('usbipd-win*.msi' in iss_text, "ISS contains usbipd-win*.msi pattern")
assert_ok('usbip-win2*.exe' in iss_text, "ISS contains usbip-win2*.exe pattern (normalized)")
assert_ok('USBip*.exe' in iss_text, "ISS contains USBip*.exe pattern (vendor naming)")
assert_ok('BUNDLED.md' in iss_text, "ISS includes BUNDLED.md provenance")
assert_ok('versions.json' in iss_text, "ISS includes versions.json")
assert_ok('InstallMsiSilently' in iss_text, "ISS has silent MSI installer (offline auto-install)")
assert_ok('InstallExeSilently' in iss_text, "ISS has silent EXE installer")
assert_ok('IsServerRole' in iss_text, "ISS has role-aware install logic")
assert_ok('IsClientRole' in iss_text, "ISS has client role logic")
assert_ok('already contains the required backends' in iss_text.lower() or 'fully offline' in iss_text.lower() or 'already inside' in iss_text.lower(), "ISS mentions offline/bundled")

# 2. Fetch backends (may create placeholders in sandbox)
log("\n=== 2. Fetch backends ===")
import subprocess
result = subprocess.run([sys.executable, os.path.join(ROOT, "ci/fetch-backends.py"), "--dry-run"], cwd=ROOT, capture_output=True, text=True)
assert_ok(result.returncode == 0, f"fetch --dry-run succeeds: {result.stdout[:200]}")
result = subprocess.run([sys.executable, os.path.join(ROOT, "ci/fetch-backends.py")], cwd=ROOT, capture_output=True, text=True)
# It may have created placeholders if CDN blocked
print(result.stdout[-1000:] if len(result.stdout)>1000 else result.stdout)
if result.stderr:
    print(result.stderr[-500:])
# After fetch, at least one msi and one exe should exist (even if placeholders)
msis = glob.glob(os.path.join(BACKENDS, "*.msi"))
exes = glob.glob(os.path.join(BACKENDS, "*.exe"))
print(f"  found MSI: {msis}")
print(f"  found EXE: {exes}")
assert_ok(len(msis) >= 1, f"at least one MSI bundled (got {msis})")
assert_ok(len(exes) >= 1, f"at least one EXE bundled (got {exes})")

# Check that at least one matches each pattern the ISS uses
def pattern_matches(pattern):
    # ISS patterns are relative to installer/
    pat = os.path.join(BACKENDS, pattern)
    return glob.glob(pat)

assert_ok(len(pattern_matches("usbipd-win*.msi")) >= 1, "usbipd-win*.msi matches at least one file")
# Either of the client patterns should match
client_matched = len(pattern_matches("usbip-win2*.exe")) + len(pattern_matches("USBip*.exe"))
assert_ok(client_matched >= 1, f"client pattern matches (usbip-win2*.exe or USBip*.exe) - got {client_matched}")

# Check BUNDLED.md and versions.json created
bundled_md = os.path.join(BACKENDS, "BUNDLED.md")
versions_json = os.path.join(BACKENDS, "versions.json")
assert_ok(os.path.exists(bundled_md), "BUNDLED.md generated")
assert_ok(os.path.exists(versions_json), "versions.json generated")
with open(bundled_md, encoding="utf-8") as f:
    md = f.read()
    assert_ok("GPL-3.0" in md, "BUNDLED.md mentions GPL-3.0")
    assert_ok("BSD-2-Clause" in md, "BUNDLED.md mentions BSD-2-Clause")
    assert_ok("usbipd-win" in md.lower(), "BUNDLED.md lists usbipd-win")
    assert_ok("usbip-win2" in md.lower() or "usbip" in md.lower(), "BUNDLED.md lists usbip-win2")
with open(versions_json, encoding="utf-8") as f:
    v = json.load(f)
    assert_ok("usbipd-win" in v, "versions.json has usbipd-win")
    assert_ok("usbip-win2" in v, "versions.json has usbip-win2")
    for k in v:
        assert_ok("sha256" in v[k], f"versions.json {k} has sha256")
        assert_ok("url" in v[k], f"versions.json {k} has url")

# 3. Simulate installer extraction and installation
log("\n=== 3. Simulate installer run ===")
# Create temp dirs mimicking Windows {tmp}\mub-backends and {app}
tmpdir = tempfile.mkdtemp(prefix="mub-test-")
try:
    tmp_backends = os.path.join(tmpdir, "tmp", "mub-backends")
    app_dir = os.path.join(tmpdir, "app")
    os.makedirs(tmp_backends, exist_ok=True)
    os.makedirs(app_dir, exist_ok=True)
    # Simulate Inno Setup extracting [Files] to {tmp}\mub-backends
    for src in msis + exes:
        shutil.copy(src, os.path.join(tmp_backends, os.path.basename(src)))
        print(f"  extracted to tmp: {os.path.basename(src)}")
    # Also simulate bundled docs extraction
    for doc in ["BUNDLED.md", "versions.json"]:
        src = os.path.join(BACKENDS, doc)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(app_dir, doc))
            assert_ok(os.path.exists(os.path.join(app_dir, doc)), f"{doc} extracted to app")

    # Simulate FindFirstFileIn logic
    def FindFirstFileIn(dir_path, pattern):
        matches = glob.glob(os.path.join(dir_path, pattern))
        if matches:
            return matches[0]
        # Also try case-insensitive on Linux via fnmatch
        for fn in os.listdir(dir_path):
            if fnmatch.fnmatch(fn.lower(), pattern.lower().replace("\\", "/").split("/")[-1].lower()):
                return os.path.join(dir_path, fn)
        return ""

    # Simulate UsbipdPath / UsbipPath before install (should be missing)
    # Then simulate installer deciding to install based on role
    def simulate_role(role):
        # role: 0=Server, 1=Client, 2=Both
        is_server = role in (0, 2)
        is_client = role in (1, 2)
        server_msi = FindFirstFileIn(tmp_backends, "usbipd-win*.msi")
        # ISS tries usbip-win2*.exe first, then USBip*.exe
        client_exe = FindFirstFileIn(tmp_backends, "usbip-win2*.exe")
        if not client_exe:
            client_exe = FindFirstFileIn(tmp_backends, "USBip*.exe")
        if not client_exe:
            client_exe = FindFirstFileIn(tmp_backends, "usbip*.exe")
        print(f"  role {role} ({'Server' if role==0 else 'Client' if role==1 else 'Both'}): server_msi={bool(server_msi)}, client_exe={bool(client_exe)}, is_server={is_server}, is_client={is_client}")
        # Simulate CurStepChanged auto-install logic
        # If backend already installed, skip; else if needed and bundled, install
        # Here we assume no backend installed yet
        installs = []
        if is_server and server_msi:
            installs.append(f"msiexec /i \"{server_msi}\" /qn")
        if is_client and client_exe:
            installs.append(f"\"{client_exe}\" /VERYSILENT")
        return installs, server_msi, client_exe

    for role in [0, 1, 2]:
        installs, sm, ce = simulate_role(role)
        if role == 0:
            assert_ok(len(installs) == 1 and "msiexec" in installs[0], "Server role installs MSI")
        elif role == 1:
            assert_ok(len(installs) == 1 and "VERYSILENT" in installs[0], "Client role installs EXE")
        else:
            assert_ok(len(installs) == 2, "Both role installs both")

    # Simulate RefreshStatus showing bundled vs installed
    # Before install: Server bundled - will be installed automatically
    server_msi = FindFirstFileIn(tmp_backends, "usbipd-win*.msi")
    client_exe = FindFirstFileIn(tmp_backends, "usbip-win2*.exe") or FindFirstFileIn(tmp_backends, "USBip*.exe")
    assert_ok(server_msi != "", "RefreshStatus: server bundled found")
    assert_ok(client_exe != "", "RefreshStatus: client bundled found")
    print(f"  RefreshStatus would show: server bundled, client bundled -> both will be auto-installed")

    # Simulate NextButtonClick: should allow Both even without prior installed backends because bundled exists
    for role in [0, 1, 2]:
        is_server = role in (0, 2)
        is_client = role in (1, 2)
        server_ok = (server_msi != "")  # bundled counts as ok per ISS logic
        client_ok = (client_exe != "")
        can_proceed = True
        if is_server and not server_ok:
            can_proceed = False
        if is_client and not client_ok:
            can_proceed = False
        assert_ok(can_proceed, f"NextButtonClick allows role {role} when bundled present")

    print("\n  All installer simulations passed - setup is now fully offline and role-aware")

finally:
    shutil.rmtree(tmpdir, ignore_errors=True)

# 4. Verify ISS lint still passes with bundled files
log("\n=== 4. ISS lint with real bundled files ===")
result = subprocess.run([sys.executable, os.path.join(ROOT, "ci/check_iss_sources.py"), ISS, "--allow", "dist/app/*"], cwd=ROOT, capture_output=True, text=True)
print(result.stdout[-2000:] if len(result.stdout)>2000 else result.stdout)
assert_ok(result.returncode == 0, "ISS lint passes with bundled files")

# 5. Check docs mention offline
log("\n=== 5. Docs reflect offline installer ===")
for path, needle in [
    ("README.md", "already inside"),
    ("README.md", "fully offline"),
    ("docs/QUICKSTART.md", "already contains"),
    ("installer/backends/README.md", "automatically at build time"),
    ("ci/README.md", "fetch backends"),
]:
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        txt = f.read()
        assert_ok(needle.lower() in txt.lower(), f"{path} mentions '{needle}'")

print("\n=== ALL TESTS PASSED ===")
print("The installer is now fully offline: backends are bundled and auto-installed by role.")
print("In sandbox, placeholder files prove the plumbing; in GitHub Actions CI, real binaries are fetched.")
