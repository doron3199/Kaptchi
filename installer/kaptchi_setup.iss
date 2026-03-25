; Kaptchi Installer — Inno Setup Script
; Builds a single KaptchiSetup.exe that installs the app, VDD, and creates shortcuts.
; VDD.Control is downloaded automatically from GitHub during installation.

#define MyAppName "Kaptchi"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Kaptchi Team"
#define MyAppExeName "kaptchi_flutter.exe"
#define VddDownloadUrl "https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VDD.Control.25.7.23.zip"
#define VddZipName "VDD.Control.25.7.23.zip"
#define VddExeName "VDD Control.exe"

[Setup]
AppId={{B8A3F2E1-7C4D-4E5A-9F6B-1D2E3F4A5B6C}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputDir=Output
OutputBaseFilename=KaptchiSetup
SetupIconFile=..\windows\runner\resources\app_icon.ico
Compression=lzma2/ultra64
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce
Name: "installvdd"; Description: "Install Virtual Display Driver (recommended for window capture)"; GroupDescription: "Optional Components:"; Flags: checkedonce

[Files]
; === Main Application ===
Source: "..\build\windows\x64\runner\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\windows\x64\runner\Release\flutter_windows.dll"; DestDir: "{app}"; Flags: ignoreversion

; === OpenCV ===
Source: "..\build\windows\x64\runner\Release\opencv_world4120.dll"; DestDir: "{app}"; Flags: ignoreversion

; === Media Server ===
Source: "..\build\windows\x64\runner\Release\mediamtx.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\windows\x64\runner\Release\mediamtx.yml"; DestDir: "{app}"; Flags: ignoreversion

; === PDF ===
Source: "..\build\windows\x64\runner\Release\pdfium.dll"; DestDir: "{app}"; Flags: ignoreversion

; === Flutter Plugin DLLs ===
Source: "..\build\windows\x64\runner\Release\camera_windows_plugin.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\windows\x64\runner\Release\permission_handler_windows_plugin.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\windows\x64\runner\Release\printing_plugin.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\windows\x64\runner\Release\screen_retriever_plugin.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\windows\x64\runner\Release\url_launcher_windows_plugin.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\windows\x64\runner\Release\window_manager_plugin.dll"; DestDir: "{app}"; Flags: ignoreversion

; === Flutter Data (assets, AOT, ICU) ===
Source: "..\build\windows\x64\runner\Release\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs

; === AI Models ===
Source: "..\build\windows\x64\runner\Release\models\*"; DestDir: "{app}\models"; Flags: ignoreversion recursesubdirs createallsubdirs

; === Python Scripts ===
Source: "..\build\windows\x64\runner\Release\scripts\*"; DestDir: "{app}\scripts"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist

[Icons]
; Desktop shortcut
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
; Start Menu
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"

[Run]
; Launch app after install (only option on final page)
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; \
  Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
// Download VDD.Control, extract it, and launch it for the user to click "Install Driver".

function DownloadFile(const Url, DestPath: String): Boolean;
var
  PsCmd: String;
  ResultCode: Integer;
begin
  PsCmd := '-NoProfile -ExecutionPolicy Bypass -Command "' +
    '[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; ' +
    'Invoke-WebRequest -Uri ''' + Url + ''' -OutFile ''' + DestPath + ''' -UseBasicParsing"';
  Result := Exec('powershell.exe', PsCmd, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) and FileExists(DestPath);
end;

function ExtractZip(const ZipPath, DestDir: String): Boolean;
var
  PsCmd: String;
  ResultCode: Integer;
begin
  ForceDirectories(DestDir);
  PsCmd := '-NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -Path ''' + ZipPath + ''' -DestinationPath ''' + DestDir + ''' -Force"';
  Result := Exec('powershell.exe', PsCmd, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

procedure InstallVddWithControl();
var
  ZipPath: String;
  VddDir: String;
  VddExePath: String;
  ResultCode: Integer;
begin
  if not WizardIsTaskSelected('installvdd') then
    Exit;

  VddDir := ExpandConstant('{app}\vdd_control');
  ZipPath := ExpandConstant('{tmp}\{#VddZipName}');

  // Step 1: Download
  WizardForm.StatusLabel.Caption := 'Downloading Virtual Display Driver...';
  WizardForm.ProgressGauge.Style := npbstMarquee;

  if not DownloadFile('{#VddDownloadUrl}', ZipPath) then
  begin
    Log('VDD download failed');
    MsgBox('Could not download Virtual Display Driver. You can install it later from the app.', mbInformation, MB_OK);
    Exit;
  end;

  // Step 2: Extract
  WizardForm.StatusLabel.Caption := 'Extracting Virtual Display Driver...';

  if not ExtractZip(ZipPath, VddDir) then
  begin
    Log('VDD extraction failed');
    MsgBox('Could not extract Virtual Display Driver. You can install it later from the app.', mbInformation, MB_OK);
    Exit;
  end;

  DeleteFile(ZipPath);

  // Step 3: Launch VDD Control for the user to install the driver
  VddExePath := VddDir + '\{#VddExeName}';

  if not FileExists(VddExePath) then
  begin
    Log('VDD Control not found at: ' + VddExePath);
    MsgBox('VDD Control not found. You can install the driver manually later.', mbInformation, MB_OK);
    Exit;
  end;

  WizardForm.StatusLabel.Caption := 'Launching VDD Control...';

  MsgBox('VDD Control will now open.' + #13#10 + #13#10 +
    'Click "Install Driver" (only ONCE), then close VDD Control to continue.', mbInformation, MB_OK);

  Exec(VddExePath, '', VddDir, SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode);

  WizardForm.ProgressGauge.Style := npbstNormal;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    InstallVddWithControl();
  end;
end;

// Force-delete the entire app folder on uninstall (catches runtime-created files like vdd_control)
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  AppDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    AppDir := ExpandConstant('{app}');
    if DirExists(AppDir) then
      DelTree(AppDir, True, True, True);
  end;
end;
