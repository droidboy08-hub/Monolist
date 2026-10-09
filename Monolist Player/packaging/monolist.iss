; Monolist's Windows installer (Inno Setup 6.3 or later; package-windows.ps1
; -Installer runs it). Everything it installs is the staged package folder,
; which package-windows.ps1 has already checked has every DLL it needs.
;
; Given on ISCC's command line:
;   /DAppVersion=0.1.130      the version, with the build number
;   /DSourceDir=<folder>      the staged package ("Monolist")
;   /DOutputDir=<folder>      where the installer goes
;   /DOutputName=<name>       its file name, without .exe
;   /DIconFile=<file.ico>     the setup program's own icon

#ifndef AppVersion
  #error AppVersion is not defined
#endif

[Setup]
; Never change: it is how Windows knows a newer installer upgrades this app.
AppId={{8E0B6F4A-5C2D-4B7E-9A31-6D2F0C8E4B19}
AppName=Monolist
AppVersion={#AppVersion}
AppVerName=Monolist {#AppVersion}
AppPublisher=Monolist
AppPublisherURL=https://github.com/droidboy08-hub/Monolist
AppSupportURL=https://github.com/droidboy08-hub/Monolist/issues
AppUpdatesURL=https://github.com/droidboy08-hub/Monolist/releases
AppCopyright=Copyright (c) 2026 droidboy08-hub (MIT)
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Monolist installer

; For this user alone by default (no administrator rights needed, in
; %LOCALAPPDATA%\Programs); the first page offers everyone on the PC instead.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\Monolist
DefaultGroupName=Monolist
DisableProgramGroupPage=yes
DisableWelcomePage=no

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\monolist.exe
UninstallDisplayName=Monolist
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; A running Monolist is closed (and offered back) rather than left holding
; files the upgrade has to replace.
CloseApplications=yes
RestartApplications=no

LicenseFile={#SourceDir}\LICENSES\Monolist-MIT.txt
InfoAfterFile={#SourceDir}\README.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; What an older version installed and this one may not: the tools and the Qt
; modules are replaced whole, so nothing stale is left to be loaded.
Type: filesandordirs; Name: "{app}\tools"
Type: filesandordirs; Name: "{app}\qml"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Monolist"; Filename: "{app}\monolist.exe"; Comment: "Monolist music player"
Name: "{autodesktop}\Monolist"; Filename: "{app}\monolist.exe"; Tasks: desktopicon

[Registry]
; Opening at sign-in is the app's own choice (Settings, Startup), never made
; here; uninstalling takes it away with the program, and Windows' note of
; whether it is allowed to start beside it.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "Monolist"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"; ValueType: none; ValueName: "Monolist"; Flags: uninsdeletevalue dontcreatekey

[Run]
Filename: "{app}\monolist.exe"; Description: "{cm:LaunchProgram,Monolist}"; Flags: nowait postinstall skipifsilent

; Uninstalling removes the program only. The library, playlists, settings and
; any sign-in (in %APPDATA% and %LOCALAPPDATA%\Monolist) and the downloads (in
; Music\Monolist) are the user's, and stay unless they delete them.
