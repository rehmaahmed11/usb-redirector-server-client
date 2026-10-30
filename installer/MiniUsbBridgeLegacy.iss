; Mini USB Bridge - legacy installer (Windows 7 SP1 ... Windows 11)
; SPDX-License-Identifier: MIT
;
; Build:  iscc installer\MiniUsbBridgeLegacy.iss       (Inno Setup 6.0 - 6.x)
; Needs:  ..\dist\app-legacy\MiniUsbBridge.exe  (.\build.ps1 -MinWin vista|xp)
;
; Why a second installer?
;   The default installer (MiniUsbBridge.iss) requires Windows 10 1809 because it
;   bundles the two USB backends, and both of them are Windows 10 only. On
;   Windows Vista/7/8/8.1 the *app* runs fine (pairing, encrypted tunnel, relay,
;   device list) but no USB backend can be installed, so this script installs the
;   app + docs, skips the backends and says so on the first page.
;
;   Windows XP cannot use this script: Inno Setup 6 refuses MinVersion < 6.0.
;   Use installer\MiniUsbBridgeXp.iss with Inno Setup 5.6.1 for XP.
;
; The application binary must be built with MUB_MIN_WIN=vista (or xp) so it does
; not import Windows-10-only APIs; see docs/COMPATIBILITY.md.

#define AppName "Mini USB Bridge"
#define AppVersion "1.0.0"
#define AppPublisher "Mini USB Bridge"
#define AppExe "MiniUsbBridge.exe"
#define AppId "{{B3E1A0F4-5C7D-4E9A-9F2B-6A1D8C3E7F52}"
#define LegacyDir "..\dist\app-legacy"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion} (legacy Windows)
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Mini USB Bridge setup (Windows 7 - 11, no bundled backends)
DefaultDirName={autopf}\Mini USB Bridge
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=MiniUsbBridge-{#AppVersion}-legacy-setup
SetupIconFile=..\src\ui\app.ico
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Windows 7 SP1 is the oldest OS Inno Setup 6 can produce a setup for.
MinVersion=6.1sp1
; 32-bit and 64-bit installs, because a Windows 7 machine may be either.
ArchitecturesAllowed=x86compatible x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
CloseApplications=yes
RestartApplications=no
AllowNoIcons=yes
ShowLanguageDialog=no
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "firewall"; Description: "Block inbound USB/IP port 3240 from the network (recommended)"; GroupDescription: "Security:"
Name: "autostart"; Description: "Start Mini USB Bridge when Windows starts"; GroupDescription: "Startup:"
Name: "installrelay"; Description: "Also install the relay helper (only if you host your own relay)"; GroupDescription: "Optional components:"; Flags: unchecked

[Files]
Source: "{#LegacyDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#LegacyDir}\mubb-relay.exe"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Tasks: installrelay
Source: "..\docs\COMPATIBILITY.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\QUICKSTART.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\NETWORKING.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\TROUBLESHOOTING.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\ARCHITECTURE.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\RELAY.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\TESTING.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\{#AppName} (Server mode)"; Filename: "{app}\{#AppExe}"; Parameters: "--mode server"
Name: "{group}\{#AppName} (Client mode)"; Filename: "{app}\{#AppExe}"; Parameters: "--mode client"
Name: "{group}\What works on this Windows version"; Filename: "{app}\docs\COMPATIBILITY.md"
Name: "{group}\Troubleshooting"; Filename: "{app}\docs\TROUBLESHOOTING.md"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Mini USB Bridge raw USB/IP"""; Flags: runhidden; Tasks: firewall
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Mini USB Bridge raw USB/IP"" dir=in action=block protocol=TCP localport=3240 profile=any"; Flags: runhidden; Tasks: firewall
Filename: "reg"; Parameters: "add ""HKCU\Software\Microsoft\Windows\CurrentVersion\Run"" /v MiniUsbBridge /t REG_SZ /d """"{app}\{#AppExe}"""" /f"; Flags: runhidden; Tasks: autostart
Filename: "{app}\docs\COMPATIBILITY.md"; Description: "Open the compatibility guide"; Flags: postinstall shellexec nowait skipifsilent unchecked
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Mini USB Bridge raw USB/IP"""; Flags: runhidden; RunOnceId: "RemoveFirewallRule"
Filename: "reg"; Parameters: "delete ""HKCU\Software\Microsoft\Windows\CurrentVersion\Run"" /v MiniUsbBridge /f"; Flags: runhidden; RunOnceId: "RemoveAutostart"

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
  MsgBox('This setup installs Mini USB Bridge without the USB backends.' + #13#10#13#10 +
         'The app itself (window, pairing, encrypted tunnel, relay, device list) runs on this ' +
         'Windows version, but the USB backends do not:' + #13#10 +
         '  - usbipd-win (SERVER) needs Windows 10 1809 x64/ARM64' + #13#10 +
         '  - usbip-win2 (CLIENT driver) needs Windows 10 1903 x64' + #13#10#13#10 +
         'The app will tell you exactly what is possible on this PC. See docs\COMPATIBILITY.md.',
         mbInformation, MB_OK);
end;
