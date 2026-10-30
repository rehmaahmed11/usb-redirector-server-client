# Troubleshooting

Everything below is written from the real failure modes of the chosen backends. Error text shown in the
app always comes from the backend or from a specific check in this code — nothing is invented.

## Backends

### "usbipd-win not usable on Windows 7, build 6.1.7601, x64" / "no client driver can run on ..."

The Windows version itself is the limit, not a missing install:

| Backend | Needs | What you can still do |
|---|---|---|
| usbipd-win (SERVER) | Windows 10 1809 x64/ARM64, Windows Server 2019+ | nothing on the device PC, unless you install a legacy `usbipd` yourself |
| usbip-win2 (CLIENT) | Windows 10 1903 x64 / Windows 11 ARM64 | only a legacy test-signed VHCI driver (for example cezanne/usbip-win v0.1.0); the app detects it, moves the tunnel to 3240 and labels it correctly |

The app logs the reason at start (`os Detected ...` / `Server backend (usbipd-win): NOT supported ...`) and
shows it in the banner. See [COMPATIBILITY.md](COMPATIBILITY.md) for every Windows version.

### "This session uses TLS 1.0 because one side runs a legacy Windows version"

An XP/Vista peer can only speak TLS 1.0. The log line `tls ... negotiated TLS 1.0` appears next to it.
Upgrade that PC, or put it behind a modern proxy. If you want to refuse old peers outright, untick
*Settings → Accept TLS 1.0/1.1 for old Windows peers* (`allowLegacyTls=0`).

### The app will not start on Windows XP ("not a valid Win32 application")

The binary was built for Windows 10. Windows XP needs a build made with `-MinWin xp`
(`build.ps1 -MinWin xp`, toolset `v141_xp`) so the PE subsystem version is 5.01 — see
[COMPATIBILITY.md](COMPATIBILITY.md#building-per-target).

### "Server backend missing: install usbipd-win"
* Install [usbipd-win](https://github.com/dorssel/usbipd-win/releases) (`winget install usbipd`).
* Verify: `where usbipd` and `sc query usbipd` (state must be `RUNNING`).
* The app looks for `usbipd.exe` in the configured path, on `PATH`, then in
  `C:\Program Files\usbipd-win`. A custom location can be set in **Settings → usbipd.exe**.
* If the service is stopped, the app asks for elevation once to run `net start usbipd`.

### "Client backend missing: install usbip-win2 (driver)"
* The client needs the **UDE virtual host controller driver** from usbip-win2. A plain `usbip.exe`
  without the driver can report success while no device node appears.
* The app detects this and refuses to claim success (see *Attach verification* below).
* usbip-win2 requires Windows 10 **1903 (build 18362)** or newer, x64. Its installer restarts USB 3.0
  hubs — do not run it during a file transfer or a call.

### "usbipd.exe was not found" while it is installed
Set the exact path in **Settings** (e.g. `C:\Program Files\usbipd-win\usbipd.exe`). Paths with spaces
are quoted automatically.

## Pairing / connection

| Message | Cause | Fix |
|---|---|---|
| `Pairing code must start with MUB1` | clipboard content is not a code | copy again from the server |
| `Pairing code is too short/truncated` | code was cut by chat apps | send it as plain text, no line breaks in the middle |
| `This pairing code has expired` | 300 s lifetime passed | **Regenerate** on the server |
| `This pairing code was already used` | single-use code | regenerate |
| `Unknown pairing code` | server regenerated while the client was typing | copy the new code |
| `Too many failed attempts` | 5 wrong attempts | regenerate on the server |
| `Server certificate does not match the pairing code` | a proxy/AV is intercepting TLS, or the code is stale (server regenerated the identity) | use the fresh code; allow the app through the security product |
| `Server authentication failed` | wrong code, or someone is impersonating the server | verify the code with the other person |
| `Access rejected by the server user` | Reject pressed or 60 s timer expired | ask again from the server |

### "Cannot reach the server: …"
The direct address inside the code was not usable. Options, in order of preference:

1. **Same LAN** — check that Windows Firewall on the server allows `MiniUsbBridge.exe` inbound (the
   first run triggers a prompt; accept it for *Private* networks).
2. **Port forward** — forward TCP `4437` (Settings → *Direct TCP port*) on the server router and put the
   public name/IP into **Settings → Public address / DDNS**, then regenerate the code.
3. **VPN/tailnet** — put the VPN address into *Public address / DDNS*.
4. **Relay** — set the same relay host/port in Settings on **both** PCs and enable *Use relay*.
   See [`RELAY.md`](RELAY.md). This is the only option that works behind two arbitrary NATs.

### "Connection Lost — Waiting for Reconnection"
The control channel dropped (Wi-Fi change, sleep, NAT timeout). The client keeps retrying with its
**session token** for up to ~20 attempts; if the server still has the approval, the session resumes
without a new pairing code and without a new Allow click. If the server was restarted or the session
expired, a new code is needed. USB devices that were attached are not silently re-attached.

## Sharing devices

* **No bus id** — the device is not visible to usbipd yet. Press **Refresh devices**; a brand-new device
  sometimes needs a second enumeration cycle.
* **`usbipd bind` says access denied** — binding needs administrator rights. The app offers a UAC
  elevation for that one operation.
* **Device disappears from the list when shared** — that is usbipd behaviour: a bound device is no longer
  usable by the server PC itself.
* **Hubs are refused** — sharing a hub would export every downstream device; share the device instead.
* **Keyboard/mouse** — sharing an input device removes local input control; the backend usually refuses.
* **A device that instantly reappears with a new VID/PID** — it re-enumerated (see the MediaTek section
  below); share the new instance.

## Attach verification

The app only reports *Attached* when all of these are true:

1. the server confirmed the export (`AttachResult = Ok`),
2. `usbip.exe attach` exited 0 with no error text,
3. a USB device with the expected VID:PID exists on the client according to SetupAPI
   (*Settings → Verify each attach in Device Manager*).

If (3) fails, the attach is rolled back and you get a specific message. Known causes:

| Message | Cause |
|---|---|
| `no USB device … appeared in Device Manager` | the usbip-win2 driver is not installed/loaded, or Windows blocked the driver |
| `usbip.exe attach failed: … device is not shared` | server-side export missing — press Share on the server |
| `usbip.exe attach failed: … already attached` | a previous session still holds the device — Detach first |
| `Server refused the data channel: Session token rejected` | the session was revoked on the server; reconnect |
| `Cannot open the local tunnel on 127.0.0.1:4240` | another program uses that port; change *Local tunnel port* |

Manual checks on the client:

```powershell
usbip.exe list -r 127.0.0.1 --tcp-port 4240          # must fail: the tunnel is token protected
usbip.exe port                                       # shows imported devices and hub ports
Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'USB\VID_0E8D*'
```

## MediaTek / SP Flash Tool

* Preloader/BROM windows are short and each one creates a **new** USB device; see
  [`QUICKSTART.md`](QUICKSTART.md#mediatek--sp-flash-tool-notes).
* The app never interrupts an attached device on its own, but a mode change is a USB disconnect — the
  flash tool sees that as a lost device. Plan the sequence so the phone enters the mode *after* the
  attach, exactly as with a local cable.
* If SP Flash Tool cannot open the port although Device Manager shows the device, the vendor driver is
  binding to a physical topology. Testing with a harmless device (USB stick/serial adapter) separates an
  app problem from a driver problem.
* Never use this tool to bypass a phone's locks or authentication; it moves USB packets only.

## Logs and diagnostics

* **View logs** (bottom right) opens `%LOCALAPPDATA%\MiniUsbBridge\logs\miniusbbridge.log`.
* Log level 0–3 in Settings (0 = debug, default 1). Pairing codes and tokens are redacted automatically.
* Backend-specific logs: usbip-win2 uses WPP tracing (see its README); usbipd-win logs to the Windows
  event log (`Microsoft-Windows-usbipd`).

## Uninstall

* Apps & features → *Mini USB Bridge*. The USB/IP backends are **not** removed (they are separate
  products, and removing the client driver restarts USB 3.0 hubs).
* Remove the firewall rule if it lingers: `netsh advfirewall firewall delete rule name="Mini USB Bridge raw USB/IP"`.
