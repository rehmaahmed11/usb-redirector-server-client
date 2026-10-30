# Testing and acceptance

This file maps every acceptance criterion to something observable, and is honest about what has been
executed and what still requires two Windows PCs and a real USB device.

## 1. Automated tests (executed in this repository, any OS)

```bash
./tools/test_core.sh
```

### Core (`tests/test_core.cpp`) — 248 checks

| Area | What is proven |
|---|---|
| SHA-256 | FIPS 180-4 vectors including the 1,000,000 × 'a' streaming case |
| HMAC-SHA256 | RFC 4231 test cases 1, 2, 3 and 6 (short and long keys) |
| Crockford base32 | reference vector for "Hello", ambiguity mapping (I/L→1, O→0), 64 random round trips |
| Pairing codes | full round trip (secret, pin, session id, host, port, flags), tolerant parsing, 6 malformed inputs rejected, unknown version rejected |
| Auth proofs | client/server proofs deterministic and domain separated, any input change breaks them |
| Pairing decision matrix | OK, wrong secret (counts as failure), wrong session, reuse, expiry, rate limit, version mismatch (no penalty), nonce replay, different code with same nonces |
| Protocol | framing split across uneven chunks, two frames in one read, device list with UTF-8 manufacturer, truncated payload refused, `ResultBody` bus id correlation, resume body |
| USB/IP header inspection | accepts 1.1.1 `OP_REQ_IMPORT` with `1-4` and `3-2.1`; refuses wrong version, wrong command, short buffer, empty busid, malformed busid, unterminated garbage |
| Strings/helpers | UTF-8↔UTF-16 with multi-byte and ™, busid validator, countdown/time formatting |

### Relay (`tests/test_relay.py`) — 10 checks

Pairing of two peers, byte splicing both directions, binary-safe payloads (all 256 byte values),
auth-tag rejection, duplicate-role rejection, malformed handshake rejection, relay still healthy after
rejections.

### Compile verification

Every translation unit compiles for `x86_64-windows-gnu` and the application links into a single
`MiniUsbBridge.exe` (verified in this repository with clang/zig targeting MSVC-compatible Windows APIs).
The MSVC build is the supported path (`build.ps1`).

## 2. Acceptance criteria — how to verify on hardware

Use two Windows 10 PCs. Mark each row when you have actually seen the stated evidence.

| # | Criterion | How to prove it | Signal |
|---|---|---|---|
| 1 | Both apps launch on Windows 10 | run the setup, start the app, switch to SERVER then back, then CLIENT | window renders, no crash, no missing DLL |
| 2 | Server detects a real USB device | plug in a USB stick | row with name, manufacturer, VID:PID appears within ~2 s; unplugging removes it |
| 3 | Server generates a pairing code | SERVER screen | `MUB1-…` visible, countdown decrements, **Copy code** puts it on the clipboard |
| 4 | Client accepts the code | paste, **CONNECT** | status *Connecting → Authenticating → Waiting for server approval* |
| 5 | Server approves the client | approval dialog | dialog shows client PC name + path; **Allow** → server shows *Client connected* |
| 6 | Connection over separate internet connections | one PC on phone hotspot, other on home LAN, relay configured | server/client show *Relay (encrypted)*; with a forwarded port, *Direct (encrypted)* |
| 7 | Client sees shared devices | server **Share** on the stick | client list shows the same VID:PID within 2 s (or after **Refresh devices**) |
| 8 | Client attaches it | **ATTACH** | banner *Attached …hub port N*; `usbip.exe port` lists the import |
| 9 | Device appears in Device Manager | `devmgmt.msc` on the client | new USB device with the same VID:PID; `Get-PnpDevice -PresentOnly` shows it |
| 10 | A normal app can use it | copy a file to the redirected stick | file transfer completes; no "device unavailable" |
| 11 | Device removal is detected | unplug on the server | server row disappears; client row shows *Removed* / the virtual device detaches |
| 12 | Invalid/expired codes rejected | paste a corrupted code, then wait 5 min and try the old one | *Pairing code must start with MUB1* / *expired*, no session |
| 13 | Unauthorized clients cannot access | press **Reject**; try `usbip.exe attach -r <server-ip>` from outside | *Access rejected*; the raw port 3240 is not reachable (firewall rule) and there is no plaintext USB/IP listener |
| 14 | Connection loss reported | disconnect the client's network during a session | *Connection Lost — Waiting for Reconnection*, then either resume or an explicit error |
| 15 | No fake attachment | on a client **without** the usbip-win2 driver, press ATTACH | error: *no USB device … appeared in Device Manager. The attach is NOT trusted*; nothing is added to Device Manager |
| 16 | `host:port` advertised address | set **Public address** to `127.0.0.1:<a forwarded port>` with a port that is not 4437, start the server | the server card shows `Address 127.0.0.1:<port>` and the client's `Direct` connect goes to that port (see the `Advertised address:` log line) |
| 17 | Free tunnel path (playit.gg or similar) | run the tunnel agent on the server PC, put `name.host:port` into **Public address**, connect from a different network | card shows `public address OK`, client shows *Direct*; unplug the tunnel and the client reports *Connection Lost — Waiting for Reconnection* rather than silently idling |
| 18 | LAN-only warning is honest | clear **Public address**, relay disabled, start the server | card shows the LAN IP with a **LAN only – set Public address in Settings** warning, and a client on another network fails with the "cannot reach" message instead of pretending to work |

Extra negative tests worth running:

* Start a second instance of the app and try to start the server → clear "port in use" message.
* Bind a device, then try to use it on the server PC → Windows reports it as unavailable (expected).
* Attach the same device twice → the client refuses with *Device is already attached*.
* Revoke on the server while attached → client shows *Connection Lost / access revoked by the server user*.
* Kill the server process → client reports the loss and retries; no ghost "Attached" state.

## 3. MediaTek checklist (Stage 7)

Prerequisites: a harmless device test (row 8–10 above) has already passed on the same pair of PCs.

| Step | Expected |
|---|---|
| Phone in normal Android/MTP mode | device visible, share + attach, Device Manager shows it |
| Phone switches to Preloader (`0E8D:2000`) | old row disappears, new row appears; client virtual device detaches; client refuses to keep claiming "Attached" |
| Share + attach the Preloader instance quickly | attach succeeds **if** the window is long enough for the round trip |
| Phone switches to BROM (`0E8D:0003`) | same as above; a new bus id |
| Start a dummy write, then unplug | the app does **not** auto-reattach and does not unbind anything; the tool sees a lost device |
| Check logs | every transition logged with timestamps, no auto-reconnect entries |

Record on the test machine whether SP Flash Tool could open the redirected port. If it cannot, note it as
a device-driver limitation (the virtual hub topology differs from the physical one) rather than as an app
failure, and include the log excerpt.

## 4. Known untested areas (be explicit with users)

1. Real USB redirection on physical hardware (rows 8–10, 15) — requires the backends and two PCs.
2. SChannel handshakes against SChannel (both ends are Windows) — code path exists and compiles; the
   relay pair test covers the byte splicing, not TLS itself.
3. Relay reliability under real WAN conditions (packet loss, MTU, long RTT).
4. usbipd-win version differences older than the current release (`state` JSON is used, with a `list`
   text fallback).
5. MediaTek driver behaviour with a virtual hub (see above).
