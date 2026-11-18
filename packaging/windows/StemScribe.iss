; Windows installer for stem-scribe (Inno Setup 6).
; Build: iscc /DAppVersion=0.1.0 /DBuild=<build dir> /DOutDir=<dir> /DModel=<ggml model file> StemScribe.iss

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef Build
  #define Build "..\..\build"
#endif
#ifndef OutDir
  #define OutDir "..\..\dist"
#endif
#ifndef Model
  #define Model "..\..\models\ggml-model-htdemucs-4s-f16.bin"
#endif
#define Art Build + "\StemScribe_artefacts\Release"

[Setup]
AppId={{6F1C2B7A-93D4-4E58-A0C2-5B7E19D3C844}
AppName=stem-scribe
AppVersion={#AppVersion}
AppPublisher=YourBrand
DefaultDirName={autopf}\YourBrand\stem-scribe
DefaultGroupName=YourBrand
DisableProgramGroupPage=yes
OutputDir={#OutDir}
OutputBaseFilename=stem-scribe-{#AppVersion}-windows-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayName=stem-scribe {#AppVersion}
InfoBeforeFile=readme-before.txt

[Types]
Name: "full"; Description: "Full install"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "vst3"; Description: "stem-scribe VST3"; Types: full custom
Name: "app"; Description: "stem-scribe standalone app"; Types: full custom
Name: "model"; Description: "Separation model (HTDemucs, about 80 MB, needed to split stems)"; Types: full custom; Flags: fixed

[Files]
Source: "{#Art}\VST3\stem-scribe.vst3\*"; DestDir: "{commoncf64}\VST3\stem-scribe.vst3"; Components: vst3; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "{#Art}\Standalone\stem-scribe.exe"; DestDir: "{app}"; Components: app; Flags: ignoreversion
Source: "{#Model}"; DestDir: "{commonappdata}\YourBrand\stem-scribe"; Components: model; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\stem-scribe"; Filename: "{app}\stem-scribe.exe"; Components: app
Name: "{autodesktop}\stem-scribe"; Filename: "{app}\stem-scribe.exe"; Components: app; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut for the standalone app"; Components: app; Flags: unchecked

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\stem-scribe.vst3"
Type: filesandordirs; Name: "{commonappdata}\YourBrand\stem-scribe"
