; Compatibility bridge for launcher-based installations. The primary release is a ZIP.

#define MyAppName "Naruto: Rise of a Ninja"
#ifndef MyAppVersion
  #include "version.iss"
#endif
#ifndef PackageDir
  #define PackageDir "..\dist\naruto-rise-recomp-win-amd64"
#endif
#ifndef SetupAppId
  #define SetupAppId "{{E8B610A5-927A-412E-9C64-42841FA6DE51}"
#endif
#ifndef LegacyUninstallKey
  #define LegacyUninstallKey "Software\Microsoft\Windows\CurrentVersion\Uninstall\NarutoRiseRecomp"
#endif
#ifndef DefaultInstallDir
  #define DefaultInstallDir "{localappdata}\NarutoRisePC"
#endif
#define MyAppPublisher "naruto-rise-recomp"
#define MyAppExeName "narutorise.exe"
#define MyAppGameExe "narutorise.exe"

#ifndef DesktopRoot
  #define DesktopRoot "{autodesktop}"
#endif
#ifndef StartMenuRoot
  #define StartMenuRoot "{group}"
#endif
#ifndef LegacyStartMenuRoot
  #define LegacyStartMenuRoot "{userprograms}\Naruto Rise of a Ninja"
#endif

[Setup]
AppId={#SetupAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={code:GetDefaultInstallDir}
UsePreviousAppDir=yes
LanguageDetectionMethod=none
UsePreviousLanguage=no
ShowLanguageDialog=yes
DisableDirPage=no
AppendDefaultDirName=no
DisableWelcomePage=yes
PrivilegesRequired=lowest
SetupIconFile=..\app\ui\narutorise.ico
UninstallDisplayIcon={app}\narutorise.exe
; The bridge installs the full current payload. Old uninstall records owned
; mods/pt-br and would delete personalized legacy files after this migration.
; Own only the current immutable payload; preserve retired resource trees.
UninstallLogMode=overwrite
DefaultGroupName=NarutoRisePC
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=NarutoRiseInstaller
Compression=lzma2/ultra64
SolidCompression=yes
SetupLogging=yes
CloseApplications=yes
RestartApplications=no
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[LangOptions]
; Inno Setup alphabetizes this dialog; visible numbers preserve the requested order.
english.LanguageName=1. English
brazilianportuguese.LanguageName=2. Português (Brasil)
french.LanguageName=3. Français
german.LanguageName=4. Deutsch
spanish.LanguageName=5. Español
italian.LanguageName=6. Italiano
russian.LanguageName=7. Русский

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Distribution binaries and DLLs
Source: "{#PackageDir}\narutorise.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\narutorise_ai2c.dll"; DestDir: "{app}"; Flags: ignoreversion
; Each character DLC engine revision needs its corresponding native module.
Source: "{#PackageDir}\narutorise_ai2c1.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#PackageDir}\narutorise_ai2c2.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#PackageDir}\narutorise_ai2c3.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#PackageDir}\rexgpu-xenos.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\rexruntime.dll"; DestDir: "{app}"; Flags: ignoreversion
; Sentry-enabled payloads require Crashpad and redistribution notices. The
; Sentry DLL is present only when the SDK was built as a separate shared library.
#if FileExists(PackageDir + "\sentry-notices.txt")
Source: "{#PackageDir}\sentry.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#PackageDir}\crashpad_handler.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\crashpad_wer.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\sentry-notices.txt"; DestDir: "{app}"; Flags: ignoreversion
#endif
; AMD FidelityFX runtime DLL - optional (only present when the build enabled
; FidelityFX). rexruntime.dll imports it, so installed copies need it to launch.
Source: "{#PackageDir}\amd_fidelityfx_dx12.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#PackageDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\shader_cache\555307E5.*"; DestDir: "{app}\shader_cache"; Flags: ignoreversion
Source: "{#PackageDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion
; Refresh immutable seeds only. Active and legacy mods remain user-owned.
Source: "{#PackageDir}\defaults\mods\pt-br\mod.toml"; DestDir: "{app}\defaults\mods\pt-br"; Flags: ignoreversion
Source: "{#PackageDir}\defaults\mods\pt-br\texts.json"; DestDir: "{app}\defaults\mods\pt-br"; Flags: ignoreversion
Source: "{#PackageDir}\defaults\narutorise.toml"; DestDir: "{app}\defaults"; Flags: ignoreversion

[Dirs]
Name: "{app}\game"; Flags: uninsneveruninstall

[Icons]
Name: "{#StartMenuRoot}\Naruto - Rise of a Ninja"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\narutorise.exe"
Name: "{#StartMenuRoot}\{cm:UninstallProgram,NarutoRisePC}"; Filename: "{uninstallexe}"
Name: "{#DesktopRoot}\Naruto - Rise of a Ninja"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\narutorise.exe"; Check: ShouldCreateDesktop

; Replace the old portable install.ps1 link only when it belongs to this copy.
Name: "{#LegacyStartMenuRoot}\Naruto - Rise of a Ninja"; Filename: "{app}\narutorise.exe"; WorkingDir: "{app}"; IconFilename: "{app}\narutorise.exe"; Check: HasLegacyStartMenuLink

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\narutorise.version"

[Messages]
brazilianportuguese.BeveledLabel=Naruto: Rise of a Ninja PC Port
english.BeveledLabel=Naruto: Rise of a Ninja PC Port
english.ConfirmUninstall=Remove %1?%n%nGame files, runtime configuration and unrelated personal files will be preserved.
brazilianportuguese.ConfirmUninstall=Remover %1?%n%nOs arquivos do jogo, a configuração e os arquivos pessoais não relacionados serão preservados.
french.BeveledLabel=Naruto: Rise of a Ninja PC Port
german.BeveledLabel=Naruto: Rise of a Ninja PC Port
spanish.BeveledLabel=Naruto: Rise of a Ninja PC Port
italian.BeveledLabel=Naruto: Rise of a Ninja PC Port
russian.BeveledLabel=Naruto: Rise of a Ninja PC Port
french.ConfirmUninstall=Supprimer %1 ?%n%nLes fichiers du jeu, la configuration et les fichiers personnels sans rapport seront conservés.
german.ConfirmUninstall=%1 entfernen?%n%nSpieldateien, Konfiguration und andere persönliche Dateien bleiben erhalten.
spanish.ConfirmUninstall=¿Eliminar %1?%n%nSe conservarán los archivos del juego, la configuración y los archivos personales no relacionados.
italian.ConfirmUninstall=Rimuovere %1?%n%nI file di gioco, la configurazione e i file personali non correlati verranno conservati.
russian.ConfirmUninstall=Удалить %1?%n%nФайлы игры, настройки и другие личные файлы будут сохранены.

[CustomMessages]
english.LauncherBusy=Close the old launcher and try again. The launcher file could not be reserved for the update.
brazilianportuguese.LauncherBusy=Feche o launcher antigo e tente novamente. Não foi possível reservar o arquivo do launcher para a atualização.
french.LauncherBusy=Fermez l'ancien lanceur et réessayez. Son fichier n'a pas pu être réservé pour la mise à jour.
german.LauncherBusy=Schließe den alten Launcher und versuche es erneut. Seine Datei konnte nicht für das Update reserviert werden.
spanish.LauncherBusy=Cierra el launcher anterior e inténtalo de nuevo. No se pudo reservar su archivo para la actualización.
italian.LauncherBusy=Chiudi il vecchio launcher e riprova. Non è stato possibile riservare il suo file per l'aggiornamento.
russian.LauncherBusy=Закройте старый лаунчер и повторите попытку. Не удалось зарезервировать его файл для обновления.
english.ConfigReadFailed=Cannot read runtime configuration: %1
english.ConfigWriteFailed=Cannot write runtime configuration: %1
english.ActionTitle=Install or update
english.ActionDescription=Choose how to install version {#MyAppVersion}.
english.ActionHint=Updates preserve your game files, configuration, saves and DLCs. Close the game and launcher before continuing.
english.UpdateOption=Update an existing installation
english.InstallOption=Install a new copy
english.UpdateFolderTitle=Select the existing installation
english.UpdateFolderDescription=Select the folder containing narutorise.exe and rexruntime.dll.
english.UpdateFolderHint=The files of the PC port will be updated in this folder. The game reads your ISO directly.
english.UpdateMissing=No existing installation was found in this folder. Select the folder containing narutorise.exe and rexruntime.dll, or choose a new installation.
english.UpdateSummary=Update existing installation: %1 -> %2
english.VersionUnknown=unknown version
english.VersionNewer=This folder contains version %1, which is newer than %2. Use a newer installer to update it.
english.VersionWriteFailed=Cannot write the installed version: %1
english.InvalidMode=Invalid setup mode. Use /MODE=update or /MODE=install.
english.UpdateFinishedTitle=Update complete
english.UpdateFinished=Version %1 has been installed. Your game files, configuration, saves and DLCs have been preserved.
brazilianportuguese.ConfigReadFailed=Não foi possível ler a configuração: %1
brazilianportuguese.ConfigWriteFailed=Não foi possível gravar a configuração: %1
brazilianportuguese.ActionTitle=Instalar ou atualizar
brazilianportuguese.ActionDescription=Escolha como instalar a versão {#MyAppVersion}.
brazilianportuguese.ActionHint=A atualização preserva os arquivos do jogo, configurações, saves e DLCs. Feche o jogo e o launcher antes de continuar.
brazilianportuguese.UpdateOption=Atualizar instalação existente
brazilianportuguese.InstallOption=Instalar uma nova cópia
brazilianportuguese.UpdateFolderTitle=Selecionar a instalação existente
brazilianportuguese.UpdateFolderDescription=Selecione a pasta que contém narutorise.exe e rexruntime.dll.
brazilianportuguese.UpdateFolderHint=Os arquivos do port para PC serão atualizados nesta pasta. O jogo lê sua ISO diretamente.
brazilianportuguese.UpdateMissing=Nenhuma instalação existente foi encontrada nesta pasta. Selecione a pasta que contém narutorise.exe e rexruntime.dll ou escolha uma nova instalação.
brazilianportuguese.UpdateSummary=Atualizar instalação existente: %1 -> %2
brazilianportuguese.VersionUnknown=versão desconhecida
brazilianportuguese.VersionNewer=Esta pasta contém a versão %1, mais recente que %2. Use um instalador mais recente para atualizá-la.
brazilianportuguese.VersionWriteFailed=Não foi possível gravar a versão instalada: %1
brazilianportuguese.InvalidMode=Modo de instalação inválido. Use /MODE=update ou /MODE=install.
brazilianportuguese.UpdateFinishedTitle=Atualização concluída
brazilianportuguese.UpdateFinished=A versão %1 foi instalada. Os arquivos do jogo, configurações, saves e DLCs foram preservados.
#include "locales\fr.iss"
#include "locales\de.iss"
#include "locales\es.iss"
#include "locales\it.iss"
#include "locales\ru.iss"

[Code]
var
  ActionPage: TInputOptionWizardPage;
  RequestedMode, DefaultDirHint: String;
  HadConfig: Boolean;
  LauncherHandle: Integer;

function ReserveLauncherFile(FileName: String; Access, Share: Cardinal;
  Security: Integer; Creation, Attributes: Cardinal; Template: Integer): Integer;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseLauncherFile(Handle: Integer): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

procedure ReleaseLauncher;
begin
  if LauncherHandle <> -1 then
  begin
    CloseLauncherFile(LauncherHandle);
    LauncherHandle := -1;
  end;
end;

function IsExistingInstallation(const Folder: String): Boolean;
begin
  Result := FileExists(AddBackslash(Folder) + '{#MyAppGameExe}') and
    FileExists(AddBackslash(Folder) + 'rexruntime.dll');
end;

function SameFolder(const Left, Right: String): Boolean;
begin
  Result := (Left <> '') and (Right <> '') and
    (CompareText(ExpandFileName(RemoveBackslashUnlessRoot(Left)),
      ExpandFileName(RemoveBackslashUnlessRoot(Right))) = 0);
end;

function GetDefaultInstallDir(Param: String): String;
var
  Folder: String;
begin
  Result := ExpandConstant('{#DefaultInstallDir}');
  if RegQueryStringValue(HKCU,
    ExpandConstant('Software\Microsoft\Windows\CurrentVersion\Uninstall\{#SetupAppId}_is1'),
    'Inno Setup: App Path', Folder) then
    if IsExistingInstallation(Folder) then
    begin
      Result := Folder;
      Exit;
    end;
  if RegQueryStringValue(HKCU, '{#LegacyUninstallKey}', 'InstallLocation', Folder) then
    if IsExistingInstallation(Folder) then Result := Folder;
end;

function IsUpdate: Boolean;
begin
  Result := ActionPage.SelectedValueIndex = 0;
end;

function InstalledVersion(const Folder: String): String;
var
  VersionText: AnsiString;
  RegisteredFolder: String;
  Key: String;
begin
  Result := '';
  if LoadStringFromFile(AddBackslash(Folder) + 'narutorise.version', VersionText) then
    Result := Trim(String(VersionText));
  if Result <> '' then Exit;
  Key := ExpandConstant('Software\Microsoft\Windows\CurrentVersion\Uninstall\{#SetupAppId}_is1');
  if RegQueryStringValue(HKCU, Key, 'Inno Setup: App Path', RegisteredFolder) then
    if SameFolder(RegisteredFolder, Folder) then
      RegQueryStringValue(HKCU, Key, 'DisplayVersion', Result);
  if Result <> '' then Exit;
  if RegQueryStringValue(HKCU, '{#LegacyUninstallKey}', 'InstallLocation', RegisteredFolder) then
    if SameFolder(RegisteredFolder, Folder) then
      RegQueryStringValue(HKCU, '{#LegacyUninstallKey}', 'DisplayVersion', Result);
end;

function NumericVersion(Version: String): String;
var
  I: Integer;
begin
  Result := Version;
  for I := 1 to Length(Version) do
    if (Version[I] = '-') or (Version[I] = '+') then
    begin
      Result := Copy(Version, 1, I - 1);
      Exit;
    end;
end;

function IsSupersededVersion(const Version: String): Boolean;
begin
  Result := (Version = '1.0.0-beta.1') or (Version = '1.0.0') or
    (Version = '1.0.1') or (Version = '1.0.2');
end;

function IsRenumberedBetaVersion(const Version: String): Boolean;
begin
  Result := (Version = '0.1.0-beta.2') or (Version = '0.1.0-beta.3') or
    (Version = '0.2.0-beta.1') or (Version = '0.2.0-beta.2') or
    (Version = '0.3.0-beta');
end;

function DestinationError: String;
var
  PreviousVersion: String;
  PreviousNumber, NewNumber: Int64;
begin
  Result := '';
  if (RequestedMode <> 'auto') and (RequestedMode <> 'update') and
    (RequestedMode <> 'install') then
  begin
    Result := CustomMessage('InvalidMode');
    Exit;
  end;
  if IsUpdate and not IsExistingInstallation(WizardDirValue) then
  begin
    Result := CustomMessage('UpdateMissing');
    Exit;
  end;
  if not IsExistingInstallation(WizardDirValue) then Exit;
  PreviousVersion := InstalledVersion(WizardDirValue);
  { Betas 2 and 3 of 0.1.0, betas 1 and 2 of 0.2.0 and 0.3.0-beta replace
    beta 1's shipped 1.0.0-beta.1 label, the old 1.0.0 label and local
    1.0.1/1.0.2 builds. Scope the numbering reset to these releases; other versions still use the
    normal downgrade check. Reject the reverse transition as well. }
  if IsRenumberedBetaVersion('{#MyAppVersion}') and
    IsSupersededVersion(PreviousVersion) then
  begin
    Log('Migrating superseded version ' + PreviousVersion + ' to {#MyAppVersion}.');
    Exit;
  end;
  if IsRenumberedBetaVersion(PreviousVersion) and
    IsSupersededVersion('{#MyAppVersion}') then
  begin
    Result := FmtMessage(CustomMessage('VersionNewer'), [PreviousVersion, '{#MyAppVersion}']);
    Exit;
  end;
  if StrToVersion(NumericVersion(PreviousVersion), PreviousNumber) and
    StrToVersion(NumericVersion('{#MyAppVersion}'), NewNumber) then
    if ComparePackedVersion(PreviousNumber, NewNumber) > 0 then
      Result := FmtMessage(CustomMessage('VersionNewer'), [PreviousVersion, '{#MyAppVersion}']);
end;

procedure InitializeWizard;
begin
  LauncherHandle := -1;
  RequestedMode := Lowercase(ExpandConstant('{param:MODE|auto}'));
  DefaultDirHint := WizardForm.SelectDirLabel.Caption;
  ActionPage := CreateInputOptionPage(wpWelcome, CustomMessage('ActionTitle'),
    CustomMessage('ActionDescription'), CustomMessage('ActionHint'), True, False);
  ActionPage.Add(CustomMessage('UpdateOption'));
  ActionPage.Add(CustomMessage('InstallOption'));
  if (RequestedMode = 'update') or
    ((RequestedMode = 'auto') and IsExistingInstallation(WizardDirValue)) then
    ActionPage.SelectedValueIndex := 0
  else ActionPage.SelectedValueIndex := 1;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpSelectDir then
  begin
    WizardForm.SelectDirLabel.Caption := DefaultDirHint;
    if IsUpdate then
    begin
      WizardForm.PageNameLabel.Caption := CustomMessage('UpdateFolderTitle');
      WizardForm.PageDescriptionLabel.Caption := CustomMessage('UpdateFolderDescription');
      WizardForm.SelectDirLabel.Caption := CustomMessage('UpdateFolderHint');
    end;
  end;
  if (CurPageID = wpFinished) and IsUpdate then
  begin
    WizardForm.FinishedHeadingLabel.Caption := CustomMessage('UpdateFinishedTitle');
    WizardForm.FinishedLabel.Caption := FmtMessage(CustomMessage('UpdateFinished'), ['{#MyAppVersion}']);
  end;
end;

{ Resolve only our known shortcuts, and preserve shortcuts to other copies. }
function OwnsShortcut(const Path: String): Boolean;
var
  Shell, Link: Variant;
  Target: String;
begin
  Result := False;
  if not FileExists(Path) then Exit;
  try
    Shell := CreateOleObject('WScript.Shell');
    Link := Shell.CreateShortcut(Path);
    Target := Link.TargetPath;
    Result := SameFolder(Target, ExpandConstant('{app}\narutorise_launcher.exe')) or
      SameFolder(Target, ExpandConstant('{app}\narutorise.exe'));
  except
    Log('Could not inspect shortcut: ' + Path);
  end;
end;

function ShouldCreateDesktop: Boolean;
begin
  Result := WizardIsTaskSelected('desktopicon') or
    OwnsShortcut(ExpandConstant('{#DesktopRoot}\Naruto - Rise of a Ninja.lnk'));
end;

function HasLegacyStartMenuLink: Boolean;
begin
  Result := OwnsShortcut(ExpandConstant('{#LegacyStartMenuRoot}\Naruto - Rise of a Ninja.lnk'));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  LegacyDir: String;
  ConfigPath, LanguageValue: String;
  Lines: TArrayOfString;
  I: Integer;
  FoundLanguage: Boolean;
begin
  if CurStep = ssPostInstall then
  begin
    { Remove only the obsolete executable, after the new files were installed. }
    ConfigPath := ExpandConstant('{app}\narutorise_launcher.exe');
    if FileExists(ConfigPath) then
      if not DeleteFile(ConfigPath) then
        RaiseException('Cannot remove the old launcher. Close it and run the bridge again.');
    ReleaseLauncher;
    if not HadConfig then
    begin
      ConfigPath := ExpandConstant('{app}\defaults\narutorise.toml');
      if ActiveLanguage = 'brazilianportuguese' then LanguageValue := 'pt_BR'
      else if ActiveLanguage = 'french' then LanguageValue := 'fr'
      else if ActiveLanguage = 'german' then LanguageValue := 'de'
      else if ActiveLanguage = 'spanish' then LanguageValue := 'es'
      else if ActiveLanguage = 'italian' then LanguageValue := 'it'
      else if ActiveLanguage = 'russian' then LanguageValue := 'ru'
      else LanguageValue := 'en';
      if not LoadStringsFromFile(ConfigPath, Lines) then
        RaiseException(FmtMessage(CustomMessage('ConfigReadFailed'), [ConfigPath]));
      FoundLanguage := False;
      for I := 0 to GetArrayLength(Lines) - 1 do
        if Pos('launcher_language', Trim(Lines[I])) = 1 then
        begin
          Lines[I] := 'launcher_language = "' + LanguageValue + '"';
          FoundLanguage := True;
        end;
      if not FoundLanguage then
      begin
        SetArrayLength(Lines, GetArrayLength(Lines) + 1);
        Lines[GetArrayLength(Lines) - 1] := 'launcher_language = "' + LanguageValue + '"';
      end;
      if not SaveStringsToUTF8File(ConfigPath, Lines, False) then
        RaiseException(FmtMessage(CustomMessage('ConfigWriteFailed'), [ConfigPath]));
    end;
    ConfigPath := ExpandConstant('{app}\narutorise.version');
    if not SaveStringToFile(ConfigPath, '{#MyAppVersion}' + #13#10, False) then
      RaiseException(FmtMessage(CustomMessage('VersionWriteFailed'), [ConfigPath]));
    { Replace only the obsolete ARP registration for THIS destination.
      Never run the old uninstaller: it could recursively delete user files. }
    if RegQueryStringValue(HKCU,
      '{#LegacyUninstallKey}',
      'InstallLocation', LegacyDir) then
      if CompareText(ExpandFileName(RemoveBackslashUnlessRoot(LegacyDir)),
        ExpandFileName(ExpandConstant('{app}'))) = 0 then
      begin
        RegDeleteKeyIncludingSubkeys(HKCU,
          '{#LegacyUninstallKey}');
        Log('Replaced legacy uninstall registration; no legacy files were deleted.');
      end;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Error: String;
begin
  Result := True;
  if WizardSilent then Exit;
  if CurPageID = wpSelectDir then
  begin
    Error := DestinationError;
    if Error <> '' then
    begin
      MsgBox(Error, mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo,
  MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  PreviousVersion: String;
begin
  PreviousVersion := InstalledVersion(WizardDirValue);
  if PreviousVersion = '' then PreviousVersion := CustomMessage('VersionUnknown');
  Result := FmtMessage(CustomMessage('UpdateSummary'), [PreviousVersion, '{#MyAppVersion}']) +
    NewLine + NewLine + MemoDirInfo + NewLine + NewLine + CustomMessage('ActionHint');
  if MemoTasksInfo <> '' then Result := Result + NewLine + NewLine + MemoTasksInfo;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := DestinationError;
  if Result <> '' then Exit;
  ReleaseLauncher;
  HadConfig := FileExists(ExpandConstant('{app}\narutorise.toml')) or
    FileExists(ExpandConstant('{app}\data\narutorise.toml'));
  if FileExists(ExpandConstant('{app}\narutorise_launcher.exe')) and (LauncherHandle = -1) then
  begin
    { Write access refuses a mapped executable; retaining it prevents a new
      launcher process from mapping the file while installation is in flight.
      Share delete allows our postinstall removal while this handle is open. }
    LauncherHandle := ReserveLauncherFile(ExpandConstant('{app}\narutorise_launcher.exe'),
      $40010000, 7, 0, 3, 0, 0);
    if LauncherHandle = -1 then Result := CustomMessage('LauncherBusy');
  end;
end;

procedure DeinitializeSetup;
begin
  ReleaseLauncher;
end;
