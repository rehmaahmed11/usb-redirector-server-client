# Security model

Scope: Mini USB Bridge moves a USB device's traffic between two PCs. Everything here is about making
sure that only the person who owns the pairing code, and who the server user has explicitly approved,
can use it.

## Assets

| Asset | Where it lives | Protection |
|---|---|---|
| Pairing secret (128 bit) | inside the pairing code | never logged, never stored, single-use, expires |
| Session token (256 bit) | memory on both ends | only over TLS, rotated on resume |
| Server TLS private key | `%LOCALAPPDATA%\MiniUsbBridge\identity.bin` | DPAPI (`CryptProtectData`), per user + machine |
| USB traffic | TLS records | TLS 1.2/1.3 with strong ciphers, certificate pinning |
| Logs | `%LOCALAPPDATA%\MiniUsbBridge\logs` | codes/tokens redacted before writing |

Nothing secret is hardcoded; no fixed password exists anywhere in the code or the installer.

## Threat model

| # | Threat | Mitigation | Verified by |
|---|---|---|---|
| 1 | Attacker connects to the server and imports a USB device | nothing is served on a public USB/IP port; the control channel requires a 128-bit secret over TLS, **and** a human Allow | `tests/test_core.cpp` (pairing matrix), code review of `HandleConnection` |
| 2 | Attacker guesses the pairing code | 128-bit secret, 5-attempt lockout, single-use, 300 s expiry | `PairingCodeExhausted` / `CheckPairingAttempt` tests |
| 3 | Replay of a used code or a captured handshake | single-use code; per-attempt random nonces (16 B each side) in the HMAC; codes invalidated on regeneration | nonce-change and reuse tests |
| 4 | Man-in-the-middle / TLS interception (corporate proxy, rogue Wi‑Fi) | SChannel + **certificate pinning**: the 10-byte SHA-256 prefix of the server certificate is inside the pairing code and enforced by the client | `Handshake()` pin comparison; failure text exposed in the UI |
| 5 | Fake server (client connects to an attacker) | the server must prove the same secret: `HMAC(secret, "MUB1-OK"‖sid‖nonceC‖nonceS)` | server-proof check test |
| 6 | Unauthenticated client pulls arbitrary busids through the tunnel | the tunnel inspects the first USB/IP message: it must be `OP_REQ_IMPORT` (0x8003) with protocol 1.1.1 and a busid that is in the client's approved set | `ParseUsbipImport` + `HandleDataChannel`, 9 header tests |
| 7 | Someone with a stolen token opens extra channels | 32-byte token, constant-time compare, token TTL 8 h, revoked on Disconnect/Stop | `ConstantTimeEqual` usage |
| 8 | Public USB/IP exposure (3240) | the local `usbipd` endpoint is only contacted on `127.0.0.1`; the client tunnel listener only accepts loopback peers (`IsLoopbackPeer`); an optional one-click firewall rule blocks inbound 3240 | `TunnelAcceptLoop`, installer task |
| 9 | Relay operator reads or tampers with USB traffic | the relay only splices bytes; TLS is end-to-end between the two PCs; a tampered stream fails the TLS MAC | `docs/RELAY.md`, `tests/test_relay.py` (relay cannot see plaintext by construction) |
| 10 | Relay session hijacking by guessing a session id | the relay join requires `HMAC(secret, "MUB1-RELAY"‖sessionId)[0..16)`; a wrong tag is rejected | `tests/test_relay.py` ("auth tag enforcement") |
| 11 | Peer floods the server | per-connection TLS handshake timeouts (15 s), pairing timeouts (12 s), frame size cap (64 KiB), rate-limit on failures, relay caps (512 sessions, 20 per sender) | code |
| 12 | Privilege escalation through the app | the GUI runs `asInvoker`; the only elevated operations are a fixed list (`bind`, `unbind`, `start-usbipd`, `firewall`, `attach`, `detach`) executed by re-launching our own signed exe via `ShellExecuteEx("runas")`; results come back through a temp file, no IPC listener | `src/win/elevated.cpp` |
| 13 | Secrets in logs / crash dumps | `Redact()` strips `MUB1-…`; tokens are never printed; identity file is DPAPI-encrypted | `src/core/logger.cpp` |

### Non-goals

* Protecting a PC that is already compromised (a local admin can always inspect memory).
* Hiding the fact that a USB device is shared from the person sitting at the server PC — that is by design.
* Bypassing any phone/device authentication. Only the USB packets the device's own driver exposes are
  forwarded.

## Protocol summary (control channel, inside TLS)

```
client → HELLO      {ver, sessionId, nonceC(16)}
server → CHALLENGE  {nonceS(16), serverName}            (set only if version/session/expiry/reuse pass)
client → AUTH       {clientName, HMAC(secret,"MUB1-AUTH"‖sid‖nonceC‖nonceS‖certPin)}
        ── human presses Allow (60 s timeout) ──
server → AUTH_RESULT{Ok, HMAC(secret,"MUB1-OK"‖sid‖nonceC‖nonceS), token(32), expires, serverName}
client → DATA_AUTH  {token}          (one data connection per usbip.exe attach)
server → DATA_OK    {Ok}
        ── raw USB/IP bytes (OP_REQ_IMPORT checked against the approved busids) ──
```

Resume after a network drop:
```
client → HELLO {ver, sessionId, nonceC} → CHALLENGE {nonceS}
client → RESUME_AUTH {token, nonceC2}
server → AUTH_RESULT {Ok, HMAC(secret,"MUB1-OK"‖sid‖nonceC‖nonceS), freshToken}
```

Framing: `type(1) | length(2, BE) | payload`, maximum payload 64 KiB, strict length checks before parsing.

## Operational recommendations

1. Keep **Block inbound USB/IP port 3240** enabled (installer task or *Settings → Firewall*).
2. Send the pairing code over a channel you trust; it is a bearer credential until it expires.
3. Press **Disconnect client** (not just *Back*) when you finish, so the approval and token die.
4. If a code leaked, press **Regenerate**: the old code, its secret and any pending approval are void.
5. Run your own relay if you do not want to depend on someone else's infrastructure.

## Reporting

This is a small project; if you find an issue, open an issue or a private advisory on the repository with
a reproducer and the affected build number.
