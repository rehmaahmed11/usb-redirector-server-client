; Mini USB Bridge - Windows XP SP3 installer (Inno Setup 5.6.1)
; SPDX-License-Identifier: MIT
;
; Build:  iscc installer\MiniUsbBridgeXp.iss
; Needs:  Inno Setup 5.6.1 (the last version that can target Windows XP -
;         Inno Setup 6 refuses any MinVersion below 6.0), and
;         ..\dist\app-legacy\MiniUsbBridge.exe built with  .\build.ps1 -MinWin xp
;
; Notes
;   * This script uses only Inno Setup 5.6 syntax on purpose: no WizardStyle,
;     no {autopf}, no ArchitecturesAllowed=x64compatible.
;   * It installs the app, the documentation and (optionally) the relay. It does
;     not install any USB backend: neither usbipd-win nor usbip-win2 can run on
;     Windows XP. See docs/COMPATIBILITY.md for what still works.

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
AppVerName={#AppName} {#AppVersion} (Windows XP)
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Mini USB Bridge setup (Windows XP SP3)
DefaultDirName={pf}\Mini USB Bridge
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=MiniUsbBridge-{#AppVersion}-xp-setup
SetupIconFile=..\src\ui\app.ico
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma
SolidCompression=yes
; Windows XP SP3 / Server 2003 and later (XP RTM/SP1/SP2 lack SHA-2 and the
; SChannel updates the app relies on).
MinVersion=5.1.2600
ArchitecturesAllowed=x86 x64
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
Source: "..\docs\RELAY.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\{#AppName} (Server mode)"; Filename: "{app}\{#AppExe}"; Parameters: "--mode server"
Name: "{group}\{#AppName} (Client mode)"; Filename: "{app}\{#AppExe}"; Parameters: "--mode client"
Name: "{group}\What works on Windows XP"; Filename: "{app}\docs\COMPATIBILITY.md"
Name: "{group}\Troubleshooting"; Filename: "{app}\docs\TROUBLESHOOTING.md"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "netsh"; Parameters: "firewall set portopening TCP 3240 ""Mini USB Bridge raw USB/IP"" DISABLE"; Flags: runhidden; Tasks: firewall; Check: IsXpOrNewerFirewall
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Mini USB Bridge raw USB/IP"""; Flags: runhidden; Tasks: firewall; Check: IsAdvancedFirewall
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Mini USB Bridge raw USB/IP"" dir=in action=block protocol=TCP localport=3240 profile=any"; Flags: runhidden; Tasks: firewall; Check: IsAdvancedFirewall
Filename: "reg"; Parameters: "add ""HKCU\Software\Microsoft\Windows\CurrentVersion\Run"" /v MiniUsbBridge /t REG_SZ /d """"{app}\{#AppExe}"""" /f"; Flags: runhidden; Tasks: autostart
Filename: "{app}\docs\COMPATIBILITY.md"; Description: "Open the compatibility guide"; Flags: postinstall shellexec nowait skipifsilent unchecked
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Mini USB Bridge raw USB/IP"""; Flags: runhidden; RunOnceId: "RemoveFirewallRule"; Check: IsAdvancedFirewall
Filename: "netsh"; Parameters: "firewall delete portopening TCP 3240"; Flags: runhidden; RunOnceId: "RemoveXpFirewallRule"; Check: not IsAdvancedFirewall
Filename: "reg"; Parameters: "delete ""HKCU\Software\Microsoft\Windows\CurrentVersion\Run"" /v MiniUsbBridge /f"; Flags: runhidden; RunOnceId: "RemoveAutostart"

[Code]
function IsAdvancedFirewall(): Boolean;
begin
  { advfirewall exists on Vista/7/8/10/11 (and on XP x64). }
  Result := FileExists(ExpandConstant('{sys}\netsh.exe')) and
            RegKeyExists(HKLM, 'SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters\FirewallPolicy\FirewallRules');
end;

function IsXpOrNewerFirewall(): Boolean;
begin
  Result := not IsAdvancedFirewall();
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  MsgBox('Mini USB Bridge on Windows XP.' + #13#10#13#10 +
         'The window, the pairing code, the encrypted tunnel and the relay work on this PC.' + #13#10 +
         'USB redirection does not: usbipd-win (SERVER) needs Windows 10 1809 x64 and the ' +
         'usbip-win2 client driver needs Windows 10 1903 x64.' + #13#10#13#10 +
         'If a legacy VHCI driver (for example cezanne/usbip-win v0.1.0) is already installed, ' +
         'the app detects it and uses port 3240 - best effort, not supported.' + #13#10#13#10 +
         'Details: docs\COMPATIBILITY.md', mbInformation, MB_OK);
end;
