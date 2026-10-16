; VolumeGuard Limiter — Windows installer (Inno Setup 6).
; Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
;
; Puts the plugin where every DAW looks for it, so nothing has to be copied by hand:
;   VST3 → C:\Program Files\Common Files\VST3\VolumeGuardLimiter.vst3
;   CLAP → C:\Program Files\Common Files\CLAP\VolumeGuardLimiter.clap
; Both formats are ticked by default; it can be removed again from "Apps" / "Programs and Features".
;
; Built in CI:  iscc /DAppVer=1.4.0 /DPkg=<folder with VST3\ CLAP\ README.md LICENSE> /DOutDir=<out> VolumeGuardLimiter.iss

#ifndef AppVer
  #define AppVer "0.0.0"
#endif
#ifndef Pkg
  #define Pkg "..\..\out"
#endif
#ifndef OutDir
  #define OutDir "."
#endif

[Setup]
; never change the AppId: it is how Windows recognises an update of the same plugin
AppId={{6F0E7C2B-3B1E-4C55-9C1A-5D2E8B7A4F31}
AppName=VolumeGuard Limiter
AppVersion={#AppVer}
AppVerName=VolumeGuard Limiter {#AppVer}
AppPublisher=Nikita Bainov (qqkillua)
AppPublisherURL=https://github.com/qqkillua/VolumeGuard
AppSupportURL=https://github.com/qqkillua/VolumeGuard/issues
AppUpdatesURL=https://github.com/qqkillua/VolumeGuard/releases
VersionInfoVersion={#AppVer}
VersionInfoDescription=VolumeGuard Limiter installer
; Common Files belongs to all users, so this needs administrator rights (one UAC prompt)
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; the readme, licence and uninstaller live here; the plugin itself goes to Common Files (see [Files])
DefaultDirName={autopf}\VolumeGuard Limiter
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
UninstallDisplayName=VolumeGuard Limiter (VST3 / CLAP)
UninstallDisplayIcon={app}\app.ico
SetupIconFile=..\..\assets\app.ico
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir={#OutDir}
OutputBaseFilename=VolumeGuardLimiter-Setup
; a DAW that has the plugin loaded keeps the file open: offer to close it, and do not restart it on our own
CloseApplications=yes
RestartApplications=no
MinVersion=10.0

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[CustomMessages]
en.Vst3=VST3 — for DAWs and Elgato Wave Link (Common Files\VST3)
ru.Vst3=VST3 — для DAW и Elgato Wave Link (Common Files\VST3)
en.Clap=CLAP — for Bitwig, REAPER, FL Studio 2024+ (Common Files\CLAP)
ru.Clap=CLAP — для Bitwig, REAPER, FL Studio 2024+ (Common Files\CLAP)

[Types]
Name: "full"; Description: "VST3 + CLAP"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "vst3"; Description: "{cm:Vst3}"; Types: full custom
Name: "clap"; Description: "{cm:Clap}"; Types: full custom

[Files]
Source: "{#Pkg}\VST3\VolumeGuardLimiter.vst3\*"; DestDir: "{commoncf64}\VST3\VolumeGuardLimiter.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Pkg}\CLAP\VolumeGuardLimiter.clap"; DestDir: "{commoncf64}\CLAP"; Components: clap; Flags: ignoreversion
Source: "{#Pkg}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Pkg}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\assets\app.ico"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; a component that was ticked off on an update must not stay behind from the previous install
Type: filesandordirs; Name: "{commoncf64}\VST3\VolumeGuardLimiter.vst3"; Check: not WizardIsComponentSelected('vst3')
Type: files; Name: "{commoncf64}\CLAP\VolumeGuardLimiter.clap"; Check: not WizardIsComponentSelected('clap')

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\VolumeGuardLimiter.vst3"
Type: files; Name: "{commoncf64}\CLAP\VolumeGuardLimiter.clap"

[Messages]
en.FinishedLabel=VolumeGuard Limiter is installed.%n%nAfter installing, let your DAW rescan its plugins (or restart it) and look for "VolumeGuard Limiter".
ru.FinishedLabel=VolumeGuard Limiter установлен.%n%nОбновите список плагинов в DAW (или перезапустите её) и найдите «VolumeGuard Limiter».
