; Inno Setup 6 script for VisualTC (Windows 10/11 x64).
; Normally run by make_installer.ps1:
;   ISCC.exe /DSourceDir=<staged files> /DOutputDir=<output> /DAppVersion=<x.y.z> visualtc.iss
; The staged folder holds VisualTC.exe, visualtc-worker.exe, the DLLs of
; GDCM/libarchive (vcpkg), what windeployqt copied and the C++ runtime.
;
; Designed for "next, next, done": no administrator password (installs for
; the current user), Portuguese or English chosen from the Windows language,
; shortcuts on the Start menu and desktop, VisualTC opens at the end.

#ifndef SourceDir
  #define SourceDir "..\..\stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif
#ifndef AppVersion
  #define AppVersion "0.2.0"
#endif
#define AppName "VisualTC"
#define AppExe "VisualTC.exe"

[Setup]
AppId={{6A4F2E31-6C1B-4B7E-9C7A-0F2D3E4B5A61}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=VisualTC
AppComments=Visualizador de imagens médicas DICOM
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Instalador do VisualTC
DefaultDirName={autopf}\VisualTC
DefaultGroupName=VisualTC
DisableProgramGroupPage=yes
DisableDirPage=auto
DisableReadyPage=yes
; Per-user installation: no UAC prompt. /ALLUSERS on the command line
; installs for every user (asks for administrator rights).
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
ShowLanguageDialog=no
LanguageDetectionMethod=uilanguage
OutputDir={#OutputDir}
OutputBaseFilename=VisualTC-Setup-x64
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
SetupIconFile=visualtc.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
ChangesAssociations=yes
CloseApplications=yes

[Languages]
; Chosen from the Windows display language (any Portuguese -> Portuguese);
; the first entry is the fallback for other languages.
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[CustomMessages]
brazilianportuguese.Integration=Integração com o Windows:
english.Integration=Windows integration:
brazilianportuguese.ContextMenuTask=Mostrar "Abrir no VisualTC" ao clicar com o botão direito em pastas, CDs/DVDs, pendrives e exames compactados (ZIP, RAR, 7z)
english.ContextMenuTask=Show "Open in VisualTC" when right-clicking folders, CDs/DVDs, USB drives and compressed exams (ZIP, RAR, 7z)
brazilianportuguese.AssocTask=Abrir arquivos .dcm com o VisualTC
english.AssocTask=Open .dcm files with VisualTC
brazilianportuguese.OpenInVisualTC=Abrir no VisualTC
english.OpenInVisualTC=Open in VisualTC
brazilianportuguese.DicomFile=Imagem médica DICOM
english.DicomFile=DICOM medical image

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "contextmenu"; Description: "{cm:ContextMenuTask}"; GroupDescription: "{cm:Integration}"
Name: "assocdcm"; Description: "{cm:AssocTask}"; GroupDescription: "{cm:Integration}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\VisualTC"; Filename: "{app}\{#AppExe}"; Comment: "{cm:DicomFile}"
Name: "{autodesktop}\VisualTC"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; HKA = HKCU for a per-user installation, HKLM with /ALLUSERS.
; --- "Abrir no VisualTC" on folders, drives (CD/DVD, USB) and archives
Root: HKA; Subkey: "Software\Classes\Directory\shell\VisualTC"; ValueType: string; ValueName: ""; ValueData: "{cm:OpenInVisualTC}"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Directory\shell\VisualTC"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\{#AppExe}"",0"; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Directory\shell\VisualTC\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Drive\shell\VisualTC"; ValueType: string; ValueName: ""; ValueData: "{cm:OpenInVisualTC}"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Drive\shell\VisualTC"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\{#AppExe}"",0"; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\Drive\shell\VisualTC\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.zip\shell\VisualTC"; ValueType: string; ValueName: ""; ValueData: "{cm:OpenInVisualTC}"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.zip\shell\VisualTC"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\{#AppExe}"",0"; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.zip\shell\VisualTC\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.rar\shell\VisualTC"; ValueType: string; ValueName: ""; ValueData: "{cm:OpenInVisualTC}"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.rar\shell\VisualTC"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\{#AppExe}"",0"; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.rar\shell\VisualTC\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.7z\shell\VisualTC"; ValueType: string; ValueName: ""; ValueData: "{cm:OpenInVisualTC}"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.7z\shell\VisualTC"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\{#AppExe}"",0"; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.7z\shell\VisualTC\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.iso\shell\VisualTC"; ValueType: string; ValueName: ""; ValueData: "{cm:OpenInVisualTC}"; Flags: uninsdeletekey; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.iso\shell\VisualTC"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\{#AppExe}"",0"; Tasks: contextmenu
Root: HKA; Subkey: "Software\Classes\SystemFileAssociations\.iso\shell\VisualTC\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: contextmenu
; --- .dcm: offered in "Abrir com"; Windows asks the user before changing the default
Root: HKA; Subkey: "Software\Classes\VisualTC.dcm"; ValueType: string; ValueName: ""; ValueData: "{cm:DicomFile}"; Flags: uninsdeletekey; Tasks: assocdcm
Root: HKA; Subkey: "Software\Classes\VisualTC.dcm\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"",0"; Tasks: assocdcm
Root: HKA; Subkey: "Software\Classes\VisualTC.dcm\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: assocdcm
Root: HKA; Subkey: "Software\Classes\.dcm\OpenWithProgids"; ValueType: string; ValueName: "VisualTC.dcm"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assocdcm
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".dcm"; ValueData: ""; Flags: uninsdeletekey; Tasks: assocdcm

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,VisualTC}"; Flags: nowait postinstall skipifsilent
