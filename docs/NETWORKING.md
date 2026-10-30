# Reaching a PC on the other side of the planet — without buying a server

You have the device on your PC. Your friend is on another continent, behind a different router, and you
do not want to rent a VPS. This page lists what actually works, in order of preference, with the exact
clicks. Facts about third-party free tiers were checked in **October 2026** — they change, so verify
before relying on them.

The app itself has two transports (`Direct` and `Relay`, see [RELAY.md](RELAY.md) and the README). What
follows is about *getting a path* for the `Direct` transport, because that is the free one.

---

## Option A — Tailscale (recommended, free, no server)

Tailscale builds a private WireGuard mesh between your machines and does the NAT traversal for you. The
free *Personal* plan covers 3 users / 100 devices with no traffic charge. It is not a relay you rent — it
is a mesh, and it also makes RDP, file sharing and `ping` work between the two PCs.

1. Install Tailscale on **both** PCs from <https://tailscale.com/download>. Sign in to both with the same
   account (or invite your friend to your tailnet).
2. On the **server PC** open a command prompt: `tailscale ip -4` → something like `100.101.102.103`.
3. In Mini USB Bridge → **Settings → Public address**, type that IP. Save.
4. Start **SERVER**, copy the pairing code, send it to your friend.
5. On the **client PC** paste the code and press **CONNECT**.

The server card now shows `Address 100.101.102.103:4437` with a green **public address OK**, meaning a
client on any network can dial it. Traffic is WireGuard-encrypted *and* TLS-encrypted end to end; nothing
is exposed to the internet at large, because a 100.x address is only reachable from inside your tailnet.

If a direct mesh connection cannot be established Tailscale falls back to its own DERP relays, which cap
throughput — fine for a serial/preloader device, slow for USB 3 mass storage.

**ZeroTier** works the same way (free plan: 25 nodes); put the assigned `10.x`/`172.x` managed address into
**Public address** instead. Any other VPN you already have (WireGuard on a router, OpenVPN, a work VPN)
also works — the app does not care, it just needs a host that resolves and a TCP port.

---

## Option B — a free outbound-only TCP tunnel (only the server PC needs it)

If your friend cannot install Tailscale (locked-down machine, no admin rights), publish the server PC's
listener through a tunnel service. Your PC makes only *outbound* connections; no port forwarding, no
public IP, works behind CGNAT.

`playit.gg` is the one with a usable free TCP tier (free: several tunnels, a random but **stable**
`name.at.playit.gg`-style address, agent only on the host side). Setup:

1. Download the playit agent on the **server PC** and claim it (it prints a link).
2. Create a **TCP** tunnel: local address `127.0.0.1`, local port `4437` (or whatever `serverPort` is).
3. playit shows something like `frosty-meadow.at.playit.gg:25565`. That `host:port` is what you need.
4. Mini USB Bridge → **Settings → Public address** → paste **`frosty-meadow.at.playit.gg:25565`**
   (scheme prefixes such as `tcp://` are accepted and stripped). Save, start **SERVER**.
5. Your friend pastes the pairing code and connects. The UI shows `Direct`.

Port 4437 in the pairing code is replaced by the tunnel's port automatically — that is exactly what the
`host:port` support in **Public address** is for.

Caveats, honestly: the tunnel operator sees your connection metadata and can drop traffic; free tiers
limit bandwidth, so a 12 Mbit/s device may feel slower; never run this without the app's own auth, which
you are not doing — the pairing code, TLS pinning and the Allow prompt are all still in the path.

Other tunnel services, so you do not waste an evening:

| Service | Free TCP tunnel? | Notes |
|---|---|---|
| playit.gg | yes | random but static address, agent only on the host PC |
| ngrok | no (needs card verification for TCP) | HTTP-only on the truly free tier |
| Cloudflare Tunnel | partly | TCP requires `cloudflared` on the **client** too, plus an account for a named tunnel |
| Pinggy / LocalXpose | no | free tiers expire after ~1 h or have no TCP at all |

Anything that gives you a `host:port` reaching `127.0.0.1:4437` on your PC will work.

---

## Option C — Windows' own RDP USB redirection (no extra app at all)

This does not use Mini USB Bridge; it is Microsoft's built-in USB-over-RDP feature. It is the answer when
you may not install *any* software on the remote PC and both PCs run Windows **Pro/Enterprise** — it does
not exist on Home.

The direction is: **the PC with the device is the RDP *client***; your friend's PC is the RDP *host*, and
the device appears in *their* Device Manager for the length of the session.

1. On **both** PCs open `gpedit.msc`:
   * Device holder PC: `Computer Configuration → Administrative Templates → Windows Components →
     Remote Desktop Services → Remote Desktop Connection Client → RemoteFX USB Device Redirection →
     Allow RDP redirection of other supported RemoteFX USB devices from this computer` → **Enabled**
     (rights: Administrators and Users).
   * Friend's PC: `... → Remote Desktop Session Host → Device and Resource Redirection → Do not allow
     supported Plug and Play device redirection` → **Disabled**.
2. `gpupdate /force` and reboot both machines (the client-side policy is not applied until reboot).
3. Connect with `mstsc` → *Show Options* → **Local Resources** → *More…* → tick the device under
   **Other supported RemoteFX USB devices**.

Limits you must know before promising this to anyone:

* Windows **Pro/Enterprise on both** ends; Home has no `gpedit` and no RDP host.
* Not for isochronous devices (webcams, USB audio) and no wireless receivers.
* The device disappears when the session ends — it is not a persistent attach.
* Microsoft has been reducing RemoteFX over time; recent builds are inconsistent. Some devices need their
  class enabled under `HKLM\SYSTEM\CurrentControlSet\Control\USBFilters`.
* The friend's PC must still accept an inbound RDP connection, so you *still* need Tailscale (Option A) or
  a forwarded port to reach it — RDP alone does not solve NAT.

For your stated use case (MediaTek preloader / BROM flashing) this option is a poor fit: the device
re-enumerates as it flashes and RemoteFX re-plugs the whole session for it. Mini USB Bridge keeps the
device exported across re-enumeration, which is why it exists.

---

## Option D — a genuinely free VPS for our own relay

If you ever want the relay and refuse to pay: **Oracle Cloud Always Free** includes an ARM VM (4 OCPU /
24 GB, permanently free, card required for sign-up verification) and **Google Cloud** has a small always-free
`e2-micro`. On it:

```bash
sudo apt install -y g++ git
git clone <this repo> && cd usb-redirector-server-client
g++ -std=c++17 -O2 -pthread relay/relay.cpp -o mubb-relay
./mubb-relay --port 4443        # open TCP 4443 in the provider's firewall / security list
```

Then set *Relay host* + port on **both** PCs and tick the relay checkbox on both. See [RELAY.md](RELAY.md).

---

## What will *not* work, so you do not try it

* **"Tunnel our USB/IP stream through mstsc / AnyDesk / TeamViewer / Parsec."** None of them exposes a
  generic TCP channel to third-party software. RDP *does* have dynamic virtual channels, but using them
  requires your own plugin on both the RDP client and the RDP host, i.e. software on the friend's PC —
  which defeats the point — plus the same Pro/Enterprise requirement.
* **UDP hole punching / STUN / ICE.** Not implemented in this app, deliberately: it needs a rendezvous
  service anyway, which is the thing you are trying not to pay for.
* **A "free public relay" run by someone else.** Anything that carries your traffic for free is either
  metered, a trial, or reading it. Tailscale is the exception because the mesh is yours and the fallback
  relays only ever see ciphertext.

## Choosing in one line

| Your situation | Use |
|---|---|
| You can install Tailscale on both PCs | **Option A** — best speed, free, nothing public |
| Friend cannot install anything | **Option B** — playit.gg tunnel on your PC only |
| Neither may install software, both Windows Pro | **Option C** — RemoteFX USB redirection over RDP + Option A for the RDP path |
| You want the app's relay, still at zero cost | **Option D** — free-tier VPS |

In every case the security does not change: the pairing code is single-use and expires, TLS is pinned to
the certificate hash inside it, and a human must press **Allow**.
