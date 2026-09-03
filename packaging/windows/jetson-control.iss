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
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile=jetson-control.ico
UninstallIconFile=jetson-control.ico
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

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
