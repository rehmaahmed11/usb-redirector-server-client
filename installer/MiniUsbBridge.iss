; Mini USB Bridge - Inno Setup 6 script (tested syntax target: Inno Setup 6.3+)
; Build:  iscc installer\MiniUsbBridge.iss     (or run installer\build.ps1)
; SPDX-License-Identifier: MIT
;
; The installer:
;   * installs MiniUsbBridge.exe (native Win32, ~1 MB) for all users
;   * asks how the PC will be used (Server / Client / Both) and creates the
;     matching shortcut + launch parameters
;   * detects the USB-over-IP backends and offers one-click download links
;     (or installs them from installer\backends\ when bundled offline)
;   * can close the raw USB/IP port 3240 to the network with a firewall rule
;   * installs the optional relay helper and the documentation

#define AppName "Mini USB Bridge"
#define AppVersion "1.0.0"
#define AppPublisher "Mini USB Bridge"
#define AppExe "MiniUsbBridge.exe"
#define AppId "{{B3E1A0F4-5C7D-4E9A-9F2B-6A1D8C3E7F52}"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Mini USB Bridge setup
DefaultDirName={autopf}\Mini USB Bridge
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=MiniUsbBridge-{#AppVersion}-setup
SetupIconFile=..\src\ui\app.ico
UninstallDisplayIcon={app}\{#AppExe}
Compression=lzma2/ultra
SolidCompression=yes
WizardStyle=modern
WizardSizePercent=110
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
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
Source: "..\dist\app\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\app\mubb-relay.exe"; DestDir: "{app}"; Flags: ignoreversion; Tasks: installrelay
Source: "..\docs\QUICKSTART.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\TROUBLESHOOTING.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\ARCHITECTURE.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\RELAY.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\TESTING.md"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
; Optional offline backends: put the vendor installers into installer\backends\
Source: "backends\usbipd-win*.msi"; DestDir: "{tmp}\mub-backends"; Flags: skipifsourcedoesntexist deleteafterinstall
Source: "backends\usbip-win2*.exe"; DestDir: "{tmp}\mub-backends"; Flags: skipifsourcedoesntexist deleteafterinstall

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\{#AppName} (Server mode)"; Filename: "{app}\{#AppExe}"; Parameters: "--mode server"
Name: "{group}\{#AppName} (Client mode)"; Filename: "{app}\{#AppExe}"; Parameters: "--mode client"
Name: "{group}\Quick start guide"; Filename: "{app}\docs\QUICKSTART.md"
Name: "{group}\Troubleshooting"; Filename: "{app}\docs\TROUBLESHOOTING.md"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Parameters: "{code:GetLaunchParams}"; Tasks: desktopicon

[Run]
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Mini USB Bridge raw USB/IP"""; Flags: runhidden; Tasks: firewall
Filename: "netsh"; Parameters: "advfirewall firewall add rule name=""Mini USB Bridge raw USB/IP"" dir=in action=block protocol=TCP localport=3240 profile=any"; Flags: runhidden; Tasks: firewall
Filename: "reg"; Parameters: "add ""HKCU\Software\Microsoft\Windows\CurrentVersion\Run"" /v MiniUsbBridge /t REG_SZ /d """"{app}\{#AppExe}"""" /f"; Flags: runhidden; Tasks: autostart
Filename: "{app}\docs\QUICKSTART.md"; Description: "Open the quick start guide"; Flags: postinstall shellexec nowait skipifsilent unchecked
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Parameters: "{code:GetLaunchParams}"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "netsh"; Parameters: "advfirewall firewall delete rule name=""Mini USB Bridge raw USB/IP"""; Flags: runhidden; RunOnceId: "RemoveFirewallRule"
Filename: "reg"; Parameters: "delete ""HKCU\Software\Microsoft\Windows\CurrentVersion\Run"" /v MiniUsbBridge /f"; Flags: runhidden; RunOnceId: "RemoveAutostart"

[Code]
var
  RolePage: TInputOptionWizardPage;
  BackendPage: TWizardPage;
  IntroLabel: TNewStaticText;
  StatusServerLabel: TNewStaticText;
  StatusClientLabel: TNewStaticText;
  ServerDownloadButton: TNewButton;
  ClientDownloadButton: TNewButton;

const
  UrlServerBackend = 'https://github.com/dorssel/usbipd-win/releases/latest';
  UrlClientBackend = 'https://github.com/vadimgrn/usbip-win2/releases/latest';

function FindFirstFileIn(const Dir, Pattern: String): String;
var
  FindRec: TFindRec;
begin
  Result := '';
  if FindFirst(AddBackslash(Dir) + Pattern, FindRec) then
  try
    repeat
      if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0 then
      begin
        Result := AddBackslash(Dir) + FindRec.Name;
        Exit;
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
end;

function BundledServerInstaller(): String;
begin
  Result := FindFirstFileIn(ExpandConstant('{tmp}\mub-backends'), 'usbipd-win*.msi');
end;

function BundledClientInstaller(): String;
begin
  Result := FindFirstFileIn(ExpandConstant('{tmp}\mub-backends'), 'usbip-win2*.exe');
end;

function FindOnPath(const ExeName: String): String;
var
  PathValue, Rest, Item, Candidate: String;
  P: Integer;
begin
  Result := '';
  PathValue := GetEnv('PATH');
  Rest := PathValue;
  while Rest <> '' do
  begin
    P := Pos(';', Rest);
    if P > 0 then
    begin
      Item := Copy(Rest, 1, P - 1);
      Rest := Copy(Rest, P + 1, Length(Rest));
    end
    else
    begin
      Item := Rest;
      Rest := '';
    end;
    Item := RemoveQuotes(Trim(Item));
    if Item <> '' then
    begin
      Candidate := AddBackslash(Item) + ExeName;
      if FileExists(Candidate) then
      begin
        Result := Candidate;
        Exit;
      end;
    end;
  end;
end;

function UsbipdPath(): String;
var
  P: String;
begin
  P := FindOnPath('usbipd.exe');
  if P = '' then
  begin
    P := ExpandConstant('{autopf}\usbipd-win\usbipd.exe');
    if not FileExists(P) then P := '';
  end;
  Result := P;
end;

function UsbipPath(): String;
var
  P: String;
begin
  P := FindOnPath('usbip.exe');
  if P = '' then
  begin
    P := ExpandConstant('{autopf}\USBip\usbip.exe');
    if not FileExists(P) then P := '';
  end;
  Result := P;
end;

procedure RefreshStatus();
var
  ServerOk, ClientOk: Boolean;
begin
  ServerOk := (UsbipdPath() <> '') or (BundledServerInstaller() <> '');
  ClientOk := (UsbipPath() <> '') or DirExists(ExpandConstant('{autopf}\USBip')) or
              (BundledClientInstaller() <> '');

  if ServerOk then
    StatusServerLabel.Caption := 'Server backend (usbipd-win):  found'
  else
    StatusServerLabel.Caption := 'Server backend (usbipd-win):  NOT installed';

  if ClientOk then
    StatusClientLabel.Caption := 'Client backend (usbip-win2 driver):  found'
  else
    StatusClientLabel.Caption := 'Client backend (usbip-win2 driver):  NOT installed';
end;

procedure OpenUrl(const Url: String);
var
  ErrorCode: Integer;
begin
  ShellExec('open', Url, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode);
end;

procedure ServerDownloadClick(Sender: TObject);
begin
  OpenUrl(UrlServerBackend);
end;

procedure ClientDownloadClick(Sender: TObject);
begin
  OpenUrl(UrlClientBackend);
end;

function GetLaunchParams(Param: String): String;
begin
  if RolePage.SelectedValueIndex = 0 then
    Result := '--mode server'
  else if RolePage.SelectedValueIndex = 1 then
    Result := '--mode client'
  else
    Result := '';
end;

procedure InitializeWizard();
begin
  RolePage := CreateInputOptionPage(wpSelectTasks,
    'How will this computer be used?',
    'Mini USB Bridge can act as a server, as a client, or both.',
    'This only decides the shortcut and the mode the app opens with. Nothing is shared and no client is ' +
    'connected automatically: you approve every client, and you pick which devices to share.',
    True, False);
  RolePage.Add('Server - USB devices are plugged into this PC and shared with a remote client');
  RolePage.Add('Client - this PC uses USB devices that are plugged into a remote server');
  RolePage.Add('Both - I will choose each time I start the app');
  RolePage.SelectedValueIndex := 2;

  BackendPage := CreateCustomPage(RolePage.ID, 'USB-over-IP backends',
    'The actual USB redirection is done by two proven open-source programs.');

  IntroLabel := TNewStaticText.Create(BackendPage);
  IntroLabel.Parent := BackendPage.Surface;
  IntroLabel.Left := 0;
  IntroLabel.Top := 0;
  IntroLabel.Width := BackendPage.SurfaceWidth;
  IntroLabel.Height := 86;
  IntroLabel.AutoSize := False;
  IntroLabel.WordWrap := True;
  IntroLabel.Caption :=
    'usbipd-win  (server side) exports the USB device.' + #13#10 +
    'usbip-win2 (client side) installs the virtual USB host controller driver, so the remote device ' +
    'really appears in Device Manager.' + #13#10#13#10 +
    'Install the one that matches your role. The client driver install may restart USB 3.0 hubs and ' +
    'can require a reboot; close file transfers and calls before you install it.';

  StatusServerLabel := TNewStaticText.Create(BackendPage);
  StatusServerLabel.Parent := BackendPage.Surface;
  StatusServerLabel.Left := 0;
  StatusServerLabel.Top := 96;
  StatusServerLabel.Width := BackendPage.SurfaceWidth;
  StatusServerLabel.Font.Style := [fsBold];
  StatusServerLabel.Caption := '';

  StatusClientLabel := TNewStaticText.Create(BackendPage);
  StatusClientLabel.Parent := BackendPage.Surface;
  StatusClientLabel.Left := 0;
  StatusClientLabel.Top := 118;
  StatusClientLabel.Width := BackendPage.SurfaceWidth;
  StatusClientLabel.Font.Style := [fsBold];
  StatusClientLabel.Caption := '';

  ServerDownloadButton := TNewButton.Create(BackendPage);
  ServerDownloadButton.Parent := BackendPage.Surface;
  ServerDownloadButton.Left := 0;
  ServerDownloadButton.Top := 152;
  ServerDownloadButton.Width := 200;
  ServerDownloadButton.Height := 30;
  ServerDownloadButton.Caption := 'Download usbipd-win (server)';
  ServerDownloadButton.OnClick := @ServerDownloadClick;

  ClientDownloadButton := TNewButton.Create(BackendPage);
  ClientDownloadButton.Parent := BackendPage.Surface;
  ClientDownloadButton.Left := 210;
  ClientDownloadButton.Top := 152;
  ClientDownloadButton.Width := 210;
  ClientDownloadButton.Height := 30;
  ClientDownloadButton.Caption := 'Download usbip-win2 (client)';
  ClientDownloadButton.OnClick := @ClientDownloadClick;

  RefreshStatus();
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = BackendPage.ID then
    RefreshStatus();
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Msg: String;
begin
  Result := True;
  if CurPageID = BackendPage.ID then
  begin
    if (UsbipdPath() = '') and (UsbipPath() = '') and (BundledServerInstaller() = '') then
    begin
      Msg := 'Neither USB/IP backend was found on this computer.' + #13#10#13#10 +
             'Mini USB Bridge will install and start, but sharing or attaching USB devices needs the ' +
             'backend for your role.' + #13#10#13#10 + 'Continue anyway?';
      if MsgBox(Msg, mbConfirmation, MB_YESNO) = IDNO then Result := False;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Name: String;
  Code: Integer;
begin
  if CurStep <> ssPostInstall then Exit;

  Name := BundledServerInstaller();
  if Name <> '' then
  begin
    if MsgBox('A bundled usbipd-win installer was found. Install it now? (required for Server mode)',
              mbConfirmation, MB_YESNO) = IDYES then
    begin
      if ShellExec('open', Name, '', '', SW_SHOWNORMAL, ewWaitUntilTerminated, Code) then
        if Code <> 0 then
          MsgBox('The usbipd-win installer exited with code ' + IntToStr(Code) + '.', mbInformation, MB_OK);
    end;
  end;

  Name := BundledClientInstaller();
  if Name <> '' then
  begin
    if MsgBox('A bundled usbip-win2 installer was found. Install it now? (required for Client mode)' + #13#10 +
              'Close other work first: USB 3.0 hubs are restarted during that installation.',
              mbConfirmation, MB_YESNO) = IDYES then
    begin
      if ShellExec('open', Name, '', '', SW_SHOWNORMAL, ewWaitUntilTerminated, Code) then
        if Code <> 0 then
          MsgBox('The usbip-win2 installer exited with code ' + IntToStr(Code) + '.', mbInformation, MB_OK);
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    MsgBox('Mini USB Bridge was removed.' + #13#10#13#10 +
           'usbipd-win and usbip-win2 were NOT removed: they are separate products. ' + #13#10 +
           'Uninstall them from "Apps & features" if you no longer need them.',
           mbInformation, MB_OK);
  end;
end;
