; Inno Setup script for VisualTC (Windows 10/11 x64)
; Build: ISCC.exe /DSourceDir=<staged files> /DOutputDir=<output> visualtc.iss
; The staged folder must contain VisualTC.exe, visualtc-worker.exe, the GDCM
; DLLs and everything windeployqt copied.

#ifndef SourceDir
  #define SourceDir "..\..\stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif
#define AppName "VisualTC"
#define AppVersion "0.1.0"

[Setup]
AppId={{6A4F2E31-6C1B-4B7E-9C7A-0F2D3E4B5A61}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=VisualTC
DefaultDirName={autopf}\VisualTC
DefaultGroupName=VisualTC
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=VisualTC-Setup-x64
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
SetupIconFile=visualtc.ico
UninstallDisplayIcon={app}\VisualTC.exe
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\VisualTC"; Filename: "{app}\VisualTC.exe"
Name: "{autodesktop}\VisualTC"; Filename: "{app}\VisualTC.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\VisualTC.exe"; Description: "{cm:LaunchProgram,VisualTC}"; Flags: nowait postinstall skipifsilent
