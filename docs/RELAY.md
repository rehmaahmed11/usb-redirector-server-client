# Running your own relay

Two PCs behind arbitrary NATs cannot reach each other without infrastructure. Mini USB Bridge therefore
uses a **relay** for that case. The relay is deliberately dumb:

* it never terminates TLS and has no keys,
* it only splices bytes between two sockets after both sides prove they know the pairing session secret,
* it is ~350 lines, has no dependencies, and runs on Windows or Linux.

```
client ──TLS──┐                  ┌──TLS── server
              ├── mubb-relay ────┤
        outbound join       outbound join
```

## Protocol

```
both sides → relay : "MUBR1" | role(1=client,2=server) | sessionId(8, BE) | authTag(16)
relay → side       : 0x00 paired   (raw bytes follow)
                     0x01 bad auth tag
                     0x02 busy / timeout / duplicate role
```

`authTag = HMAC-SHA256(pairingSecret, "MUB1-RELAY" ‖ sessionId)[0..16)`. Because the pairing secret is
128-bit random and single-use, a third party cannot join or hijack a session id even though session ids
are visible to the relay operator.

The relay keeps a session for at most 5 minutes of waiting, caps concurrent sessions (512) and rejects a
second socket claiming a role that is already connected.

## Build

Windows:

```powershell
cmake --build build --config Release --target mubb-relay
:: or: cl /std:c++17 /O2 relay\relay.cpp ws2_32.lib
```

Linux (handy for a small VPS):

```bash
g++ -std=c++17 -O2 -pthread relay/relay.cpp -o mubb-relay
```

## Run

```bash
./mubb-relay --port 4443 --verbose
```

Put it behind TLS-terminating infrastructure only if you like; it expects plain TCP. Firewall rule: allow
inbound TCP 4443.

### systemd example

```ini
[Unit]
Description=Mini USB Bridge relay
After=network-online.target

[Service]
ExecStart=/usr/local/bin/mubb-relay --port 4443
Restart=always
User=mubrelay
DynamicUser=yes
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

## Configure both PCs

Settings → **Relay**:

| Field | Server PC | Client PC |
|---|---|---|
| Use relay when a direct connection is impossible | ✔ | ✔ |
| Relay host | `relay.example.com` or the IP | same |
| Port | `4443` | same |

The pairing code contains a flag that a relay is available; if the direct connection fails the client
falls back to the relay automatically, and the UI shows *Relay (encrypted)* for that session.

Mode of operation: the client joins with its session id; the server keeps a waiting join for its current
pairing code. When the code is regenerated, a new relay session id is used.

**One relay join per connection, not per session.** The server sits in a loop: join as role 2, wait, get
paired, hand the socket to the normal connection handler, then join again. The control channel takes the
first pair; every USB/IP data channel the client opens (one per attached device, created by the client's
127.0.0.1 tunnel) takes the next. That is why a wedged data channel never blocks the control channel —
but it also means the relay sees one pairing per attached device, and the server must stay in the loop
for as long as a client may attach something.

Because the relay asks both sides to prove knowledge of the pairing secret before it splices anything, a
client that only knows the *session id* (which is 64-bit and visible to the relay operator) still cannot
join. The TLS handshake that follows is what actually protects the data, and it is pinned to the
certificate hash inside the pairing code.

## Sizing

Splicing costs ~2 copies per direction; a small VPS (1 vCPU, 512 MB) handles a USB 2.0 full-speed device
(12 Mbit/s) comfortably. High-bandwidth devices (USB 3 mass storage) are limited by the relay's bandwidth
and by added latency — prefer a direct/VPN path for those.

## What the relay operator can and cannot see

* Can see: connection metadata (IPs, timestamps, session ids, byte counts).
* Cannot see: the pairing secret, the session token, device names, or any USB payload — all of that is
  inside TLS that terminates on the two PCs.
* Can do: drop or delay traffic (a denial of service). It cannot inject undetected data because TLS
  integrity checks fail on the endpoints.
