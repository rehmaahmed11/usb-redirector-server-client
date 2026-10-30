# Mini USB Bridge — Architecture & Backend Validation

Status: **architecture validated from upstream source/docs; hardware validation checklist in `docs/TESTING.md` is not yet executed.**
Nothing in this repository claims that USB redirection has been tested on real hardware yet.

---

## 1. Backend investigation (the decisive question)

Four candidate projects were inspected (upstream READMEs, CLI parsers, protocol constants, installer docs).

| Project | Role on Windows | Language | License | Last activity | Verdict |
|---|---|---|---|---|---|
| [dorssel/usbipd-win](https://github.com/dorssel/usbipd-win) | **USB/IP server** (exports local USB devices) | C# (.NET) | **GPL-3.0** | active (2026‑10) | **Use as Server engine** |
| [vadimgrn/usbip-win2](https://github.com/vadimgrn/usbip-win2) | **USB/IP client** (UDE virtual host controller) | C++ | **BSD-2-Clause** | active (2026‑10) | **Use as Client engine** |
| [ren2r/usbip-win](https://github.com/ren2r/usbip-win) | fork of usbip-win | none detected | GPL-3.0 | dead since 2021‑01 | Rejected (unmaintained, no releases, no Win10 driver story) |
| [ameggs/OpenUSBIP](https://github.com/ameggs/OpenUSBIP) | userland USB-over-IP (Python) | Python | NOASSERTION | 2026‑05, 3 stars | Rejected (no Windows kernel driver ⇒ device can never appear in Device Manager; not the USB/IP protocol) |

### Why the split (server = usbipd-win, client = usbip-win2) is the only viable combination

* **usbipd-win is server-only by design.** Its README: *"Client-side tooling exists for other operating systems such as Microsoft Windows, but not as part of this project."* It ships a Windows service `usbipd` (TCP 3240) and a CLI (`list`, `bind`, `unbind`, `attach --wsl`, `state`). It exposes devices over the USB/IP protocol version **0x0111 (1.1.1)** — verified in `Usbipd/Interop/UsbIp.cs` (`USBIP_VERSION = 0x0111`).
* **usbip-win2 is client-only by design.** It ships a WHLK-certified / attestation-signed **UDE** (USB Device Emulation) driver `usbip2_ude` + upper filter `usbip2_filter`, so a remote device really does enumerate as a USB device on the client (Device Manager, normal Win32 apps, and USB stacks in general see it). Its stated requirement: *"The USB/IP server must support protocol version 1.1.1"* — which is exactly what usbipd-win announces. Requirement: Windows 10 x64 **1903 (18362)** or later.
* **Protocol wire compatibility matters, not branding.** Both ends speak the Linux USB/IP protocol; the busid string the client asks for (`OP_REQ_IMPORT`) is the one the server exports, so no format translation is needed.
* **Neither project can do both halves**; no single Windows project implements both a USB/IP server *and* a signed virtual host controller.

### Licensing consequences (must be respected)

`usbipd-win` is **GPL-3.0**. This project therefore **never links** it. It is invoked as a **separate executable** (`usbipd.exe`) over a process boundary — a separate program, not a derived work. The same is true for `usbip-win2` (BSD-2-Clause), which is invoked as `usbip.exe`. Our own code stays under MIT (see `LICENSE`), and we do not redistribute or modify either binary:

* The installer **detects** the backends and offers one-click download links; it can also bundle the vendor installers (`installer/backends/`) if the operator wants a single offline package. Bundling is allowed because each vendor installer is redistributable as-is (GPL-3 → source offer requirements apply; keep the vendor `.msi`/`setup.exe` unmodified).
* Administrator rights, the `usbipd` service, the UDE driver, and a possible **reboot** are vendor requirements, surfaced in the installer and in the app's Diagnostics panel.

### Known limitations of the chosen backend pair (stated up front)

1. **`usbipd bind` takes the device away from the local host.** While a device is bound, the Windows machine that physically owns it loses access to it. The app binds only on explicit user action and unbinds on "Stop sharing"/disconnect.
2. **Re-enumeration destroys the session.** If a device re-enumerates (unplug, or a phone switching to Preloader/BROM), the old instance disappears: the ACL/session ends, the client's virtual device detaches. Our app detects this (`WM_DEVICECHANGE`) and offers *manual* re-attach. It never auto-restarts anything.
3. **usbipd-win cannot export a device that never stays present.** MediaTek Preloader/BROM windows can be shorter than a WAN round trip (see §7).
4. **Client needs Windows 10 1903+ and a driver install** (with a USB 3.0 hub restart during installation). No way around that — it is a kernel driver.
5. **USB/IP is TCP-only**, no encryption and no authentication. This is the core reason for this project: we never expose 3240. See §4.

---

## 2. Product architecture

```
  SERVER PC                                     CLIENT PC
 ┌──────────────────────────────┐              ┌──────────────────────────────┐
 │ MiniUsbBridge.exe (server)   │              │ MiniUsbBridge.exe (client)   │
 │                              │              │                              │
 │ UI (Win32)  Settings/Logs    │              │ UI (Win32)  Settings/Logs    │
 │        │                     │              │        │                     │
 │ UsbDetection (SetupAPI)      │              │ ClientMode (state machine)   │
 │ DeviceMonitor (WM_DEVICECHANGE)             │        │                     │
 │        │                     │              │ UsbipClient (usbip.exe)      │
 │ UsbipdCli (usbipd.exe)       │              │        │                     │
 │        │  bind/unbind/state  │              │        │ attach/detach       │
 │        ▼                     │              │        ▼                     │
 │  usbipd service :3240 ───────┼──┐           │  UDE driver (usbip2_ude)     │
 │  (loopback only!)            │  │           │        │ 127.0.0.1:3240      │
 └──────────────────────────────┘  │           └────────┼─────────────────────┘
                                   │                    │
                    127.0.0.1:3240 │              LocalTunnel (loopback
                                   │              listener, byte pump)
                                   ▼                    ▲
                     ┌──────────────────────────────────┴────┐
                     │  Mini USB Bridge secure channel       │
                     │  TLS 1.2/1.3 (SChannel) + pinning     │
                     │  pairing-key HMAC auth, session token │
                     ├───────────────────────────────────────┤
                     │ direct TCP  ──── or ────  relay       │
                     │ (host:port from pairing code)         │
                     └───────────────────────────────────────┘
                                   ▲
                     mubb-relay (own server, documented
                     in docs/RELAY.md; untrusted for
                     confidentiality, sees only TLS)
```

Key property: **USB/IP never leaves loopback.** The only thing on the wire is our TLS channel. There is no public USB/IP port, no plaintext USB traffic, and no unauthenticated listener anywhere.

### Data path for one ATTACH

1. Client user clicks ATTACH → `RequestAttach{busid}` on the control channel.
2. Server verifies the device is in the client's allowed set → `usbipd bind --busid=1-2` (elevated helper if needed) → replies `AttachResult{ok}`.
3. Client app starts (if not running) its **loopback tunnel** on `127.0.0.1:<tcpPort>` (default 4240, chosen because `usbip.exe` has a global `-t/--tcp-port` option; range 1024–65535).
4. Client runs `usbip.exe -t 4240 attach -r 127.0.0.1 -b 1-2`.
5. The UDE driver opens a TCP connection to `127.0.0.1:4240`; our tunnel performs a **fresh TLS connection** to the server, authenticates with the client's session token, and then blind-pipes bytes.
6. Server tunnel connects to `127.0.0.1:3240` (usbipd) and blind-pipes. The USB/IP `OP_REQ_IMPORT` (containing the busid) arrives from the driver and is forwarded verbatim.
7. `usbip.exe` reports the hub port; the device enumerates on the client. Success is only reported **after** both the CLI exit code *and* a SetupAPI sighting of the expected VID/PID.

---

## 3. Pairing & authentication design

### 3.1 Pairing code

A 128-bit session secret plus routing information, rendered as a copy-paste string. No passwords, no hardcoded keys, no shared static secret.

```
wire payload (big-endian):
  u8   version (=1)
  u8   flags            bit0 preferDirect, bit1 relayAvailable, bit2 lanOnly
  u16  directPort
  u32  expiresUnix      (UTC seconds; server rejects after this)
  u10  certPin          (80-bit truncated SHA-256 of the server's TLS cert DER)
  u64  sessionId        (random, 64-bit)
  u128 secret           (random; HMAC key for this session)
  u16  hostLen + host   (ASCII, IP or DNS name, ≤ 64 chars)
```
Encoded with **Crockford base32** (no I/L/O/U ambiguity), grouped as `MUB1-XXXXX-XXXXX-…`; the parser ignores dashes/space/case and maps `I,L→1`, `O→0`.

Properties: single-use by default (server marks the session used after the first successful AUTH), expiry enforced server-side (default 5 min, configurable), regeneration invalidates the previous code (in-memory + the secret is replaced), and the code alone is useless without the human **Allow** click.

### 3.2 Challenge/response over TLS

```
client → HELLO      {ver, sessionId, nonce_c(16)}
server → CHALLENGE  {nonce_s(16), serverName}
client → AUTH       {clientName, hmac_sha256(secret, "MUB1-AUTH" || sessionId || nonce_c || nonce_s || certPin)}
server → AUTH_RESULT{status, hmac_sha256(secret, "MUB1-OK" || nonce_c || nonce_s), token(32B), expires}
```
* The server's response HMAC proves the server also holds the secret (mutual auth over the pinned TLS channel).
* `AUTH_RESULT` is only sent after the **local human approves** (dialog, 60 s countdown → auto-reject).
* The issued **session token** (32 random bytes) is what data channels use: `DATA_AUTH{token}` → `DATA_OK`. USB bytes are never sent before `DATA_OK`.
* TLS pinning: the client's SChannel verify callback accepts the server certificate **only** if the truncated SHA-256 matches the pin in the pairing code. Combined with HMAC, a MITM must break TLS *and* know the secret.
* Relay-only mode still authenticates exactly the same way (the relay is a dumb byte splicer).

### 3.3 Brute force / abuse resistance

* 128-bit secret ⇒ code guessing is infeasible; additionally the server rate-limits pairing attempts (5 failures ⇒ code burned, session locked, log entry).
* Codes expire (default 300 s) and are single-use.
* Only approved clients get device lists; unknown clients get a rejection without any device metadata.
* All control messages are length-checked (max 64 KiB) and type-checked before parsing.

---

## 4. Connectivity: direct, NAT traversal, relay

| Path | How | When |
|---|---|---|
| **Direct** | Code carries `host:port`; client connects straight to the server's TLS listener | Same LAN, public IP, port forwarded, or any VPN/tailnet the user already runs |
| **Relay** (NAT traversal) | Both sides make **outbound** connections to `mubb-relay`; the relay joins them by `sessionId` | Default when neither side has an inbound port — the common case |
| VPN | WireGuard/Tailscale/ZeroTier address placed in Settings | Optional, works with direct mode |

**Honest statement:** this project does **not** implement UDP hole punching / STUN / ICE. Two arbitrary NATs cannot be connected without *some* infrastructure; the relay is that infrastructure, and it is documented and self-hostable (`relay/mubb-relay.cpp`, ~250 lines, `docs/RELAY.md`). The app shows which path is in use ("Direct" / "Relay") and never claims direct connectivity when it is relaying.

Relay protocol (byte-splicing only, no crypto on the relay):
```
client→relay: "MUBR1" 0x01 sessionId(8B) authTag(16B=HMAC(secret,"RELAY")[0..16])
server→relay: "MUBR1" 0x02 sessionId(8B) authTag(16B)
relay: pairs both, then copies bytes both ways until EOF; 0x00 ok / 0x01 bad tag / 0x02 timeout
```
The relay authenticates with the pairing secret (so random parties cannot hijack a session id), cannot read or modify the TLS stream undetected, and can be rate-limited.

---

## 5. Threading & UI model

* UI thread: pure Win32, owner-drawn flat controls, no blocking calls. All state changes arrive as queued `WM_APP_*` messages carrying small POD payloads.
* Server: 1 accept thread (TLS handshake per connection in that connection's own thread), N session threads, 1 device-watchdog thread running `usbipd state` every 2 s.
* Client: 1 control-channel thread + reconnection logic, 1 loopback tunnel accept thread + 1 thread per data channel.
* Every socket op has timeouts (connect 8 s, handshake 10 s, idle 45 s + keepalive every 15 s).
* No USB byte ever touches the UI thread.

---

## 6. USB detection

* `SetupAPI` + `GUID_DEVINTERFACE_USB_DEVICE` enumeration for devices, plus parent-hub walk for **busid-like** location, names (`SPDRP_FRIENDLYNAME`), manufacturer (`SPDRP_MFC`), `SPDRP_HARDWAREID` for VID/PID.
* `RegisterDeviceNotification(DBT_DEVICEARRIVAL/DBT_DEVICEREMOVECOMPLETE)` filters for live updates; signature-checked.
* **Only real USB devices** are listed (device interface class), never empty root/hub ports. Hubs themselves are filtered out of the shareable list.
* `usbipd state` (JSON, machine-readable, official) is joined by `InstanceId` to obtain the USB/IP `busid` and sharing state; `usbipd list` text parsing exists only as a fallback for older versions.
* Sharing rules: a device can be shared with exactly **one** session at a time; requests for an already-attached device are refused with a clear error; the client's attach call is verified and the UI shows the real backend message on failure.

---

## 7. MediaTek / Android flashing analysis (important)

Real sequence for SP Flash Tool: `normal Android` → `Preloader` (VID 0E8D/PID 2000) → `BROM` (VID 0E8D/PID 0003) → device resets, often several times, each time **with a new device instance and new busid, and with the previous node disappearing sometimes in under a second**. usbipd-win binds a *specific device instance*; when it disappears the binding and the import are gone, and usbip-win2's virtual device detaches.

Therefore, truthfully:

* **USB redirection works for stable devices** (storage, serial, HID, phones in normal MTP/ADB mode, programmer boxes that stay enumerated).
* **Interactive flashing of an MTK phone across the WAN is not reliable.** Not because of a bug in this app, but because Preloader/BROM windows are short, reset-driven, and each transition destroys the export. The USB/IP server cannot survive device teardown.
* What the app *does* provide for this workflow:
  * immediate detection of the transition and a clear banner ("Device re-enumerated — previous attach is gone");
  * a **Session guard** that never issues detach/unbind while bytes are flowing, so a *successful* flash is not interrupted by our own housekeeping;
  * no automatic re-attach, exactly as required;
  * honest error reporting from the backend, never a fake "attached".
* Recommended for flashing: same LAN or a wired/VPN path (low, stable RTT), and *manually* re-attach after each mode change. If the device list shows a new VID/PID (e.g. `0E8D:2000` → `0E8D:0003`), that is the transition, and it must be re-shared and re-attached.

The app will not attempt to bypass any authentication/lock state of the phone; it only moves USB packets that the phone's own driver stack exposes.

---

## 8. Modules (as built)

```
src/
  main.cpp              entry point, single-instance, mode dispatch, elevated ops
  core/                 portable, unit-tested on any host
    bytes.h             ByteWriter/ByteReader
    sha256.{h,cpp}      SHA-256 + HMAC-SHA256 (RFC 6234 / RFC 4231 vectors in tests)
    base32.{h,cpp}      Crockford base32
    pairing.{h,cpp}     pairing code encode/decode/validate
    protocol.{h,cpp}    message framing + all payload codecs
    strings.{h,cpp}     utf8/utf16, hex, formatting
    logger.{h,cpp}      ring buffer + file sink + redaction
    settings.{h,cpp}    INI in %LOCALAPPDATA%
  win/
    winutil.{h,cpp}     DPI, fonts, owner-draw helpers, message queue helpers
    crypto_win.{h,cpp}  BCryptGenRandom, constant-time compare
    net.{h,cpp}         Winsock wrappers, timeouts, loopback checks
    tls.{h,cpp}         SChannel client+server, cert creation/PFX, pinning
    elevated.{h,cpp}    ShellExecuteEx runas helper + result file
  server/
    usb_detect.{h,cpp}  SetupAPI enumeration
    device_monitor.{h,cpp}
    usbipd_cli.{h,cpp}
    server_mode.{h,cpp}
  client/
    usbip_cli.{h,cpp}
    local_tunnel.{h,cpp}
    client_mode.{h,cpp}
  ui/
    resource.h, app.rc, manifest
    main_window.{h,cpp}
    server_view.{h,cpp}
    client_view.{h,cpp}
    settings_dialog.{h,cpp}
    approval_dialog.{h,cpp}
relay/mubb-relay.cpp    self-hostable relay
tests/test_core.cpp     portable crypto/pairing/protocol tests
installer/MiniUsbBridge.iss (Inno Setup 6)
```

## 9. Acceptance mapping

`docs/TESTING.md` maps every acceptance criterion to an observable signal in the app
(e.g. "device appears in Device Manager" ⇒ ATTACH finished **and** a new USB node with the
remote VID/PID exists on the client, verified by `tools/list-usb.ps1`).
