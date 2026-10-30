# Mini USB Bridge — Quick start

Two PCs, two internet connections. No port forwarding, no VPS and no paid service is needed — see
[NETWORKING.md](NETWORKING.md) for the free ways to get a path between the two machines (a Tailscale mesh
is the easiest, a free TCP tunnel the most locked-down-friendly).

## 0. Install the pieces

Check what your Windows versions can do first — the full matrix is in
[COMPATIBILITY.md](COMPATIBILITY.md). Short version: the app runs from Windows XP SP3 up, but the two
backends (and therefore USB redirection) need Windows 10 1809 (server) and Windows 10 1903 (client driver).

Just run the single offline installer on **both PCs**: `MiniUsbBridge-1.0.0-setup.exe`
(Windows 10 1809+). On Windows 7 SP1 - 8.1 use `MiniUsbBridge-1.0.0-legacy-setup.exe`, on Windows XP
`MiniUsbBridge-1.0.0-xp-setup.exe`; both install the app and docs only, because no backend can run on
those Windows versions.

The installer **already contains the required backends** (official vendor releases, unmodified):

| PC | Backend bundled in the installer | Why |
|---|---|---|
| **Server** | [usbipd-win](https://github.com/dorssel/usbipd-win/releases) (GPL-3.0) | exports the USB device (`winget install usbipd` is the manual equivalent) |
| **Client** | [usbip-win2](https://github.com/vadimgrn/usbip-win2/releases) (BSD-2-Clause) | installs the signed UDE virtual USB host controller driver |

During setup choose **Server / Client / Both** - the needed backend is installed automatically.
Already installed backends are left alone. Provenance (version, SHA-256) is in `{app}\docs\BUNDLED.md`.

The installer's *USB-over-IP backends* page still shows the status (a red banner appears only if a
backend is missing **and** not bundled, which only happens with dev builds). The app's start screen
also shows the status.

usbip-win2 notes: it restarts USB 3.0 hubs during installation (close file transfers/calls first) and
may require a reboot. Without that driver the remote device can never appear in Device Manager — the
client will say so instead of pretending.

## 1. Server PC (the one with the phone/device)

1. Start Mini USB Bridge → **SERVER**.
2. Status shows *Waiting for Client* and a pairing code like
   `MUB1-7K4QM-2X8TP-…`. Press **Copy code**.
3. Plug in the USB device. It appears in the list with name, manufacturer, VID:PID and status.

## 2. Client PC

1. Start Mini USB Bridge → **CLIENT**.
2. Paste the code (or press **Paste**) → **CONNECT**.
3. Status goes *Connecting → Authenticating → Waiting for server approval*.

## 3. Approve on the server

A dialog appears: *"A client is requesting access to your USB devices"*, showing the client's PC name,
its address and the connection path. Press **Allow** (or **Reject**; it auto-rejects after 60 s).
Nobody can get in without this click.

## 4. Share a device on the server, attach it on the client

* Server: press **Share** on the row of the device (or **Share all**). The row turns *Shared*.
* Client: press **ATTACH** on that device.
  Mini USB Bridge asks the server for authorization, then calls `usbip.exe attach` and — this is the
  important part — **verifies with SetupAPI that a USB device with the remote VID:PID actually appeared**.
  Only then does it show *Attached*. If the check fails you get an error like:
  `usbip.exe reported success, but no USB device 0e8d:2000 appeared in Device Manager. The attach is NOT trusted`.

The device is now in Device Manager on the client and can be used by normal Windows software.

## 5. Ending the session

* Client: **DETACH** on the row, or **Disconnect** (detaches everything).
* Server: **Disconnect client** (revokes access and generates a fresh code), **Stop sharing** on a row,
  or **Back** to stop the server.

Closing the app detaches everything it attached.

---

## Settings worth knowing

| Setting | Default | Meaning |
|---|---|---|
| Direct TCP port | 4437 | the port the server listens on for the encrypted channel |
| Pairing code lifetime | 300 s | the code expires and a new one is generated automatically |
| Single-use code | on | one successful pairing per code |
| Relay host / port | empty / 4443 | set both on both PCs to enable NAT traversal through your relay |
| Public address | empty | a reachable host or `host:port`: DDNS name, Tailscale IP, or a tunnel address — see [NETWORKING.md](NETWORKING.md) |
| Local tunnel port | 4240 | loopback port handed to `usbip.exe --tcp-port` |
| Verify attach in Device Manager | on | never report "attached" without a real device node |
| Block inbound 3240 (Settings → Firewall) | — | closes the raw USB/IP port to the network (needs UAC once) |

Settings live in `%LOCALAPPDATA%\MiniUsbBridge\settings.ini`; logs in
`%LOCALAPPDATA%\MiniUsbBridge\logs\miniusbbridge.log` (open it with **View logs**).

---

## MediaTek / SP Flash Tool notes

A MediaTek phone is not one USB device but a sequence of them:

| Mode | Typical VID:PID | Behaviour |
|---|---|---|
| Android (MTP/ADB) | `0e8d:201d`, `0e8d:2008`, … | stable, stays enumerated |
| Preloader | `0e8d:2000` | appears for a short window, then disappears |
| BROM | `0e8d:0003` | appears after the phone resets, again short-lived |

Every transition removes the old Windows device and enumerates a new one, which means:

1. the server's export for the old instance is gone,
2. the client's virtual device detaches,
3. a new bus id appears in the server list.

So the workflow is **manual re-attach per mode change**:

1. On the server, keep the device list visible. When the phone resets, the old row disappears and a new
   row (new VID:PID, new bus id) arrives within ~2 seconds.
2. Press **Share** on the new row.
3. On the client press **Refresh devices**, then **ATTACH** on the new row.

Rules the app follows so it never breaks a running flash by itself:

* it never detaches, unbinds or re-shares a device automatically;
* it never auto-attaches after a re-enumeration;
* failures are reported with the backend's own message;
* no authentication or security bypass of the phone is implemented (only USB packets that the phone's
  own driver exposes are moved).

**Expectation management:** Preloader/BROM windows can be shorter than a WAN round trip. Even with a
perfect setup, interactive flashing over a long-distance link may miss the mode window. Same-LAN or
VPN/WireGuard paths have low enough latency to work far more reliably than relayed connections. Devices
that stay enumerated (fastboot, a stable VCOM port, mass storage) are the reliable cases. This is a
property of USB/IP + MediaTek's reset behaviour, not something this app can hide.

Additionally, some MediaTek/VCOM drivers bind to a specific USB topology; a virtual hub port can look
different to them. If SP Flash Tool cannot open the port even though Device Manager shows it, the
device driver is the limitation — test with a harmless device (USB stick, serial adapter) first.

---

## Troubleshooting in one minute

| Symptom | Check |
|---|---|
| Banner: *Server backend missing* | install usbipd-win, then `sc query usbipd` → RUNNING |
| Banner: *Client backend missing* | install usbip-win2 and reboot |
| *Cannot listen on TCP port 4437* | another instance is running, or change the port in Settings |
| *Cannot reach the server* | verify the code is fresh; for separate networks enable the relay on both sides (Settings) |
| *Pairing code expired / already used* | press **Regenerate** on the server, copy again |
| *Access rejected by the server user* | the server pressed Reject or the 60 s timer expired |
| *USB/IP attach failed: …* | read the backend text: usually "device is not shared" or a driver problem |
| Device visible but app can't open it | try a USB 2.0 port, avoid hubs, and check the vendor driver on the client |
| *Connection Lost — Waiting for Reconnection* | network dropped; the client retries with its session token and resumes without a new code |

More: [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md).
