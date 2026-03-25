; Kaptchi Installer — Inno Setup Script
; Builds a single KaptchiSetup.exe that installs the app and optionally the VDD driver.
; VDD driver files are bundled in the build output (vdd_driver/).

#define MyAppName "Kaptchi"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Kaptchi Team"
#define MyAppExeName "kaptchi_flutter.exe"

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
DisableDirPage=yes
DisableWelcomePage=no
DisableReadyPage=yes
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

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

; === VDD Driver Files ===
Source: "..\build\windows\x64\runner\Release\vdd_driver\*"; DestDir: "{app}\vdd_driver"; Flags: ignoreversion

[Icons]
; Desktop shortcut
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
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
// Install the bundled VDD driver using pnputil (files are in {app}\vdd_driver\).

procedure InstallVddDriver();
var
  InfPath: String;
  ScriptPath: String;
  PsArgs: String;
  ResultCode: Integer;
begin
  InfPath := ExpandConstant('{app}\vdd_driver\MttVDD.inf');

  if not FileExists(InfPath) then
  begin
    Log('VDD .inf not found at: ' + InfPath);
    MsgBox('VDD driver files not found. You can install it later from the app.', mbInformation, MB_OK);
    Exit;
  end;

  WizardForm.StatusLabel.Caption := 'Installing Virtual Display Driver...';
  WizardForm.ProgressGauge.Style := npbstMarquee;

  // Write a PowerShell script that: adds driver to store, creates device node if needed, enables it
  ScriptPath := ExpandConstant('{tmp}\install_vdd.ps1');
  SaveStringToFile(ScriptPath,
    '$ErrorActionPreference = "Stop"' + #13#10 +
    '$infPath = "' + InfPath + '"' + #13#10 +
    'Write-Host "Adding driver to store..."' + #13#10 +
    'pnputil /add-driver $infPath /install' + #13#10 +
    '$dev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains "ROOT\MttVDD" }' + #13#10 +
    'if ($dev) {' + #13#10 +
    '    Write-Host "Device exists, enabling..."' + #13#10 +
    '    Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction SilentlyContinue' + #13#10 +
    '    Start-Sleep -Seconds 1' + #13#10 +
    '    Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false' + #13#10 +
    '} else {' + #13#10 +
    '    Write-Host "Creating device node..."' + #13#10 +
    '    Add-Type @"' + #13#10 +
    'using System; using System.Runtime.InteropServices;' + #13#10 +
    'public class DevInst {' + #13#10 +
    '    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]' + #13#10 +
    '    static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid g, IntPtr h);' + #13#10 +
    '    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]' + #13#10 +
    '    static extern bool SetupDiCreateDeviceInfoW(IntPtr s, string n, ref Guid g, string d, IntPtr h, uint f, ref SP i);' + #13#10 +
    '    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]' + #13#10 +
    '    static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr s, ref SP i, uint p, byte[] b, uint l);' + #13#10 +
    '    [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]' + #13#10 +
    '    static extern bool SetupDiCallClassInstaller(uint f, IntPtr s, ref SP i);' + #13#10 +
    '    [DllImport("setupapi.dll", SetLastError=true)]' + #13#10 +
    '    static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);' + #13#10 +
    '    [DllImport("newdev.dll", CharSet=CharSet.Unicode, SetLastError=true)]' + #13#10 +
    '    static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr h, string id, string inf, uint fl, out bool r);' + #13#10 +
    '    [StructLayout(LayoutKind.Sequential)] public struct SP { public uint cbSize; public Guid ClassGuid; public uint DevInst; public IntPtr Reserved; }' + #13#10 +
    '    public static bool Install(string inf) {' + #13#10 +
    '        Guid g = new Guid("{4d36e968-e325-11ce-bfc1-08002be10318}");' + #13#10 +
    '        IntPtr s = SetupDiCreateDeviceInfoList(ref g, IntPtr.Zero);' + #13#10 +
    '        if (s == (IntPtr)(-1)) return false;' + #13#10 +
    '        SP i = new SP(); i.cbSize = (uint)Marshal.SizeOf(i);' + #13#10 +
    '        if (!SetupDiCreateDeviceInfoW(s,"MttVDD",ref g,null,IntPtr.Zero,1,ref i)){SetupDiDestroyDeviceInfoList(s);return false;}' + #13#10 +
    '        byte[] h = System.Text.Encoding.Unicode.GetBytes("ROOT\\MttVDD\0\0");' + #13#10 +
    '        if (!SetupDiSetDeviceRegistryPropertyW(s,ref i,1,h,(uint)h.Length)){SetupDiDestroyDeviceInfoList(s);return false;}' + #13#10 +
    '        if (!SetupDiCallClassInstaller(0x19,s,ref i)){SetupDiDestroyDeviceInfoList(s);return false;}' + #13#10 +
    '        SetupDiDestroyDeviceInfoList(s); bool r;' + #13#10 +
    '        UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero,"ROOT\\MttVDD",inf,1,out r);' + #13#10 +
    '        return true;' + #13#10 +
    '    }' + #13#10 +
    '}' + #13#10 +
    '"@' + #13#10 +
    '    $result = [DevInst]::Install($infPath)' + #13#10 +
    '    Write-Host "Device creation: $result"' + #13#10 +
    '}' + #13#10 +
    'Write-Host "Done"' + #13#10, False);

  PsArgs := '-NoProfile -ExecutionPolicy Bypass -File "' + ScriptPath + '"';
  Log('Running VDD install script...');

  if Exec('powershell.exe', PsArgs, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Log('VDD install script exit code: ' + IntToStr(ResultCode))
  else
    Log('Failed to run VDD install script');

  DeleteFile(ScriptPath);
  WizardForm.ProgressGauge.Style := npbstNormal;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    InstallVddDriver();
  end;
end;

// Clean up on uninstall
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
