; Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; Developed at the German Cancer Research Center (DKFZ).

#define MyAppName "Jetson Control"
#define MyAppPublisher "DKFZ"
#define MyAppExeName "jetson-control.exe"

#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#ifndef MySourceDir
  #define MySourceDir "..\..\build\package\bin"
#endif
#ifndef MyOutputDir
  #define MyOutputDir "..\..\dist"
#endif
#ifndef MyOutputBaseFilename
  #define MyOutputBaseFilename "jetson-control-windows-x86_64-setup"
#endif

[Setup]
; Keep this ID stable so later system-wide installers update the same app.
AppId={{23B365E7-7611-4F73-B355-A92B8397E055}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\Jetson Control
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputDir={#MyOutputDir}
OutputBaseFilename={#MyOutputBaseFilename}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile=jetson-control.ico
UninstallIconFile=jetson-control.ico
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
#ifdef MySignTool
SignTool={#MySignTool}
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "{#MySourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceDir}\..\vc_redist.x64.exe"; Flags: dontcopy

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\{#MyAppExeName}"; ValueType: string; ValueData: "{app}\{#MyAppExeName}"; Flags: uninsdeletekey

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
var
  RuntimeNeedsRestart: Boolean;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  RuntimePath, RequiredVersion, InstalledVersion: String;
  RequiredPacked, InstalledPacked: Int64;
  Installed: Cardinal;
  ResultCode: Integer;
begin
  Result := '';
  ExtractTemporaryFile('vc_redist.x64.exe');
  RuntimePath := ExpandConstant('{tmp}\vc_redist.x64.exe');
  if not GetVersionNumbersString(RuntimePath, RequiredVersion) or
     not StrToVersion(RequiredVersion, RequiredPacked) then
  begin
    Result := 'Could not determine the bundled Visual C++ runtime version.';
    Exit;
  end;

  if RegQueryDWordValue(HKLM64, 'Software\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed) and
     (Installed = 1) and
     RegQueryStringValue(HKLM64, 'Software\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Version', InstalledVersion) then
  begin
    if Copy(InstalledVersion, 1, 1) = 'v' then
      Delete(InstalledVersion, 1, 1);
    if StrToVersion(InstalledVersion, InstalledPacked) and
       (ComparePackedVersion(InstalledPacked, RequiredPacked) >= 0) then
      Exit;
  end;

  if not Exec(RuntimePath, '/install /quiet /norestart', '', SW_HIDE,
              ewWaitUntilTerminated, ResultCode) then
    Result := 'Could not start the Visual C++ runtime installer.'
  else if ResultCode = 3010 then
    RuntimeNeedsRestart := True
  else if ResultCode <> 0 then
    Result := Format('Visual C++ runtime installation failed (code %d).', [ResultCode]);
end;

function NeedRestart: Boolean;
begin
  Result := RuntimeNeedsRestart;
end;
