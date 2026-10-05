; Inno Setup Script para Naruto: Rise of a Ninja (PC Recompilation Port)
; Requisitos: Inno Setup 6.4+ (ExecAndLogOutput).

#define MyAppName "Naruto: Rise of a Ninja"
#ifndef MyAppVersion
  #include "version.iss"
#endif
#ifndef PackageDir
  #define PackageDir "..\dist\naruto-rise-recomp-win-amd64"
#endif
#ifndef HelperPath
  #define HelperPath "..\app\out\build\win-amd64-release\narutorise_setup_helper.exe"
#endif
#ifndef SetupAppId
  #define SetupAppId "{{E8B610A5-927A-412E-9C64-42841FA6DE51}"
#endif
#ifndef PortBytes
  #define PortBytes FileSize(PackageDir + "\narutorise.exe") + FileSize(PackageDir + "\narutorise_ai2c.dll") + FileSize(PackageDir + "\rexruntime.dll") + FileSize(PackageDir + "\rexgpu-xenos.dll") + FileSize(PackageDir + "\narutorise_launcher.exe")
#endif
#ifndef LegacyUninstallKey
  #define LegacyUninstallKey "Software\Microsoft\Windows\CurrentVersion\Uninstall\NarutoRiseRecomp"
#endif
#define MyAppPublisher "naruto-rise-recomp"
#define MyAppExeName "narutorise_launcher.exe"
#define MyAppGameExe "narutorise.exe"

[Setup]
AppId={#SetupAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\NarutoRisePC
UsePreviousAppDir=yes
DisableDirPage=no
DisableWelcomePage=yes
PrivilegesRequired=lowest
SetupIconFile=..\launcher\assets\narutorise.ico
UninstallDisplayIcon={app}\assets\narutorise.ico
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
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Binários e DLLs da distribuição
Source: "{#PackageDir}\narutorise.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\narutorise_launcher.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\narutorise_ai2c.dll"; DestDir: "{app}"; Flags: ignoreversion
; DLC engine module - optional (only present when the build included DLC
; support). Without it the game runs normally, but a DLC install requires it.
Source: "{#PackageDir}\narutorise_ai2c2.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#PackageDir}\rexgpu-xenos.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\rexruntime.dll"; DestDir: "{app}"; Flags: ignoreversion
; AMD FidelityFX runtime DLL - optional (only present when the build enabled
; FidelityFX). rexruntime.dll imports it, so installed copies need it to launch.
Source: "{#PackageDir}\amd_fidelityfx_dx12.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#PackageDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\shader_cache\555307E5.*"; DestDir: "{app}\shader_cache"; Flags: ignoreversion
Source: "..\launcher\assets\cover.jpg"; DestDir: "{app}\assets"; Flags: ignoreversion
Source: "..\launcher\assets\narutorise.ico"; DestDir: "{app}\assets"; Flags: ignoreversion
Source: "..\launcher\assets\fonts\*"; DestDir: "{app}\assets\fonts"; Flags: ignoreversion
Source: "..\tools\extract-xiso\LICENSE.TXT"; DestDir: "{app}\licenses"; DestName: "extract-xiso.txt"; Flags: ignoreversion
Source: "{#PackageDir}\narutorise.toml"; DestDir: "{app}"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "..\tools\extract-xiso\extract-xiso.exe"; Flags: dontcopy
Source: "{#HelperPath}"; Flags: dontcopy
Source: "{tmp}\narutorise_iso\*"; DestDir: "{app}\{code:GetImportStageName}"; Flags: external ignoreversion recursesubdirs createallsubdirs uninsneveruninstall; Check: IsoExtracted

[Dirs]
Name: "{app}\game"; Flags: uninsneveruninstall

[Icons]
Name: "{group}\Naruto - Rise of a Ninja"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\assets\narutorise.ico"
Name: "{group}\{cm:UninstallProgram,NarutoRisePC}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Naruto - Rise of a Ninja"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\assets\narutorise.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Messages]
brazilianportuguese.BeveledLabel=Naruto: Rise of a Ninja PC Port
english.BeveledLabel=Naruto: Rise of a Ninja PC Port
english.ConfirmUninstall=Remove %1?%n%nGame files, runtime configuration and unrelated personal files will be preserved.
brazilianportuguese.ConfirmUninstall=Remover %1?%n%nOs arquivos do jogo, a configuração e os arquivos pessoais não relacionados serão preservados.

[CustomMessages]
english.IsoTitle=Select Game ISO
english.IsoDescription=Choose your original Naruto: Rise of a Ninja Xbox 360 ISO.
english.IsoHint=Setup can extract your ISO into the game folder. Leave this field empty to use already extracted files. You can run Setup again to import an ISO later. Game files are not included.
english.IsoLabel=ISO file:
english.IsoFilter=ISO images (*.iso)|*.iso|All files (*.*)|*.*
english.IsoMissing=The selected ISO file does not exist.
english.IsoSkipped=(skipped - use extracted files or run Setup again later)
english.ExtractTitle=Extracting game files
english.ExtractDescription=Please wait while your ISO is extracted. This may take several minutes.
english.ExtractFailed=Could not extract the ISO. Check the image and available disk space. Existing game files have not been changed.
english.IsoConflict=The destination contains game data or other files in game. To protect them, leave the ISO field empty or choose another installation folder.
english.CheckTitle=Checking game ISO
english.CheckDescription=Checking the game identity and reading the file list...
english.WrongGame=This image is not Naruto: Rise of a Ninja for Xbox 360 (Title ID 555307E5).
english.InvalidIso=The image is not a readable Xbox 360 ISO. Check your original game dump.
english.ImportCanceled=ISO import canceled. No existing game files have been changed. You can retry or install without an ISO.
english.Canceling=Canceling ISO import. Please wait...
english.SpaceError=Not enough free space, or available space could not be determined. Check the installation and temporary drives.
english.SpaceSummary=Game data: %1 MB. Additional space is required for temporary extraction and copying.
english.Preserved=Game files and configuration will be preserved when uninstalling.
english.ExistingXex=Found default.xex in the destination. Existing game files will not be replaced.
english.ExistingOther=The destination has files in game, but no default.xex. They will not be deleted automatically.
english.EmptyFolder=An empty game folder exists. You can import your ISO here.
brazilianportuguese.IsoTitle=Selecionar ISO do jogo
brazilianportuguese.IsoDescription=Escolha sua ISO original de Naruto: Rise of a Ninja para Xbox 360.
brazilianportuguese.IsoHint=O instalador pode extrair sua ISO para a pasta game. Deixe o campo vazio para usar arquivos já extraídos. Você pode executar o instalador novamente para importar uma ISO depois. Os arquivos do jogo não estão incluídos.
brazilianportuguese.IsoLabel=Arquivo ISO:
brazilianportuguese.IsoFilter=Imagens ISO (*.iso)|*.iso|Todos os arquivos (*.*)|*.*
brazilianportuguese.IsoMissing=O arquivo ISO selecionado não existe.
brazilianportuguese.IsoSkipped=(ignorada - usar arquivos extraídos ou executar o instalador novamente)
brazilianportuguese.ExtractTitle=Extraindo arquivos do jogo
brazilianportuguese.ExtractDescription=Aguarde enquanto sua ISO é extraída. Isso pode levar vários minutos.
brazilianportuguese.ExtractFailed=Não foi possível extrair a ISO. Verifique a imagem e o espaço livre em disco. Os arquivos existentes do jogo não foram alterados.
brazilianportuguese.IsoConflict=O destino contém dados do jogo ou outros arquivos em game. Para protegê-los, deixe o campo ISO vazio ou escolha outra pasta de instalação.
brazilianportuguese.CheckTitle=Verificar ISO do jogo
brazilianportuguese.CheckDescription=Verificando a identidade do jogo e lendo a lista de arquivos...
brazilianportuguese.WrongGame=Esta imagem não é Naruto: Rise of a Ninja para Xbox 360 (Title ID 555307E5).
brazilianportuguese.InvalidIso=A imagem não é uma ISO Xbox 360 legível. Verifique o dump do seu jogo original.
brazilianportuguese.ImportCanceled=Importação da ISO cancelada. Os arquivos existentes não foram alterados. Você pode tentar novamente ou instalar sem uma ISO.
brazilianportuguese.Canceling=Cancelando a importação da ISO. Aguarde...
brazilianportuguese.SpaceError=Não há espaço livre suficiente ou não foi possível consultá-lo. Verifique as unidades de instalação e de arquivos temporários.
brazilianportuguese.SpaceSummary=Dados do jogo: %1 MB. Também é necessário espaço para extração temporária e cópia.
brazilianportuguese.Preserved=Os arquivos do jogo e a configuração serão preservados na desinstalação.
brazilianportuguese.ExistingXex=Foi encontrado default.xex no destino. Os arquivos existentes do jogo não serão substituídos.
brazilianportuguese.ExistingOther=O destino contém arquivos em game, mas não default.xex. Eles não serão apagados automaticamente.
brazilianportuguese.EmptyFolder=Existe uma pasta game vazia. Você pode importar sua ISO aqui.

[Code]
var
  IsoPage: TInputFileWizardPage;
  ExtractPage: TOutputProgressWizardPage;
  Extracted: Boolean;
  HelperRunning: Boolean;
  CancelRequested: Boolean;
  CancelImportButton: TNewButton;
  IsoBytes: Int64;
  IsoFiles: Integer;
  HelperError: String;
  HelperPhase: String;
  ExistingDataLabel: TNewStaticText;
  HadConfig: Boolean;
  ImportStageName: String;
  ImportStageOwned: Boolean;
  ImportPublished: Boolean;

function IsoExtracted: Boolean;
begin
  Result := Extracted;
end;

function GetImportStageName(Param: String): String;
begin
  Result := ImportStageName;
end;

function GetFileAttributesW(Name: String): Cardinal;
  external 'GetFileAttributesW@kernel32.dll stdcall';

function GameRootIsOccupied: Boolean;
var
  Root: String;
  Find: TFindRec;
begin
  Root := ExpandConstant('{app}\game');
  Result := FileExists(Root);
  if not DirExists(Root) then Exit;
  { Never follow an existing junction or symbolic link, even if empty. }
  if (GetFileAttributesW(Root) and $400) <> 0 then
  begin
    Result := True;
    Exit;
  end;
  if FindFirst(Root + '\*', Find) then
  begin
    try
      repeat
        if (Find.Name <> '.') and (Find.Name <> '..') then Result := True;
      until Result or not FindNext(Find);
    finally
      FindClose(Find);
    end;
  end;
end;

procedure CancelImport(Sender: TObject);
begin
  CancelRequested := True;
  SaveStringToFile(ExpandConstant('{tmp}\cancel-iso'), 'cancel', False);
  CancelImportButton.Enabled := False;
  ExtractPage.SetText(CustomMessage('Canceling'), '');
end;

procedure InitializeWizard;
begin
  ImportStageName := '.narutorise-import-' + ExtractFileName(ExpandConstant('{tmp}'));
  IsoPage := CreateInputFilePage(wpSelectDir, CustomMessage('IsoTitle'),
    CustomMessage('IsoDescription'), CustomMessage('IsoHint'));
  IsoPage.Add(CustomMessage('IsoLabel'), CustomMessage('IsoFilter'), '.iso');
  ExtractPage := CreateOutputProgressPage(CustomMessage('ExtractTitle'),
    CustomMessage('ExtractDescription'));
  CancelImportButton := TNewButton.Create(WizardForm);
  CancelImportButton.Parent := ExtractPage.Surface;
  CancelImportButton.Caption := SetupMessage(msgButtonCancel);
  CancelImportButton.SetBounds(ExtractPage.SurfaceWidth - ScaleX(90),
    ExtractPage.ProgressBar.Top + ExtractPage.ProgressBar.Height + ScaleY(20),
    ScaleX(90), ScaleY(25));
  CancelImportButton.OnClick := @CancelImport;
  IsoPage.Values[0] := ExpandConstant('{param:GAMEISO|}');
  ExistingDataLabel := TNewStaticText.Create(WizardForm);
  ExistingDataLabel.Parent := IsoPage.Surface;
  ExistingDataLabel.AutoSize := False;
  ExistingDataLabel.WordWrap := True;
  ExistingDataLabel.SetBounds(0, IsoPage.Edits[0].Top + IsoPage.Edits[0].Height + ScaleY(16),
    IsoPage.SurfaceWidth, ScaleY(60));
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = IsoPage.ID then
  begin
    ExistingDataLabel.Caption := '';
    if FileExists(ExpandConstant('{app}\game\default.xex')) then
      ExistingDataLabel.Caption := CustomMessage('ExistingXex')
    else if GameRootIsOccupied then
      ExistingDataLabel.Caption := CustomMessage('ExistingOther')
    else if DirExists(ExpandConstant('{app}\game')) then
      ExistingDataLabel.Caption := CustomMessage('EmptyFolder');
  end;
end;

procedure HelperOutput(const S: String; const Error, FirstLine: Boolean);
var
  Fields: TStringList;
  Done, Total: Integer;
begin
  Log(S);
  Fields := TStringList.Create;
  try
    Fields.Delimiter := '|';
    Fields.StrictDelimiter := True;
    Fields.DelimitedText := S;
    if Fields.Count >= 2 then
    begin
      if Fields[0] = 'BYTES' then IsoBytes := StrToInt64Def(Fields[1], 0);
      if Fields[0] = 'FILES' then IsoFiles := StrToIntDef(Fields[1], 0);
      if Fields[0] = 'ERROR' then HelperError := Fields[1];
      if (Fields[0] = 'PROGRESS') and (Fields.Count >= 4) then
      begin
        Done := StrToIntDef(Fields[1], 0);
        Total := StrToIntDef(Fields[2], 0);
        if not CancelRequested then
          ExtractPage.SetText(HelperPhase, Fields[3]);
        if Total > 0 then
        begin
          ExtractPage.ProgressBar.Style := npbstNormal;
          ExtractPage.SetProgress(Done, Total + 1)
        end
        else
        begin
          ExtractPage.ProgressBar.Style := npbstMarquee;
          ExtractPage.SetProgress(0, 0);
        end;
      end;
    end;
  finally
    Fields.Free;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  LegacyDir: String;
  ConfigPath, LanguageValue: String;
  Lines: TArrayOfString;
  I: Integer;
  FoundLanguage: Boolean;
  StagePath, GamePath: String;
begin
  if (CurStep = ssInstall) and Extracted then
  begin
    StagePath := ExpandConstant('{app}\') + ImportStageName;
    if DirExists(StagePath) or FileExists(StagePath) then
      RaiseException('Import staging destination already exists: ' + StagePath);
    if not ForceDirectories(StagePath) then
      RaiseException('Cannot create import staging destination: ' + StagePath);
    ImportStageOwned := True;
  end;
  if CurStep = ssPostInstall then
  begin
    if Extracted then
    begin
      StagePath := ExpandConstant('{app}\') + ImportStageName;
      GamePath := ExpandConstant('{app}\game');
      if GameRootIsOccupied then RaiseException(CustomMessage('IsoConflict'));
      if not FileExists(StagePath + '\default.xex') then
        RaiseException(CustomMessage('ExtractFailed'));
      { Only an empty destination may be removed. Publish by same-volume rename,
        never expose partial game data while copying or during cancellation. }
      if DirExists(GamePath) then
        if not RemoveDir(GamePath) then RaiseException(CustomMessage('IsoConflict'));
      if not RenameFile(StagePath, GamePath) then
        RaiseException(CustomMessage('ExtractFailed'));
      ImportPublished := True;
    end;
    if not HadConfig then
    begin
      ConfigPath := ExpandConstant('{app}\narutorise.toml');
      if ActiveLanguage = 'brazilianportuguese' then LanguageValue := 'pt_BR'
      else LanguageValue := 'en';
      if not LoadStringsFromFile(ConfigPath, Lines) then
        RaiseException('Cannot read runtime configuration: ' + ConfigPath);
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
        RaiseException('Cannot write runtime configuration: ' + ConfigPath);
    end;
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

procedure DeinitializeSetup;
begin
  { This unique staging folder was created by this setup invocation only. }
  if ImportStageOwned and not ImportPublished then
    DelTree(ExpandConstant('{app}\') + ImportStageName, True, True, True);
end;

function RunHelper(Params, Title, Description: String): Boolean;
var
  ExitCode: Integer;
begin
  Result := False;
  HelperError := '';
  CancelRequested := False;
  DeleteFile(ExpandConstant('{tmp}\cancel-iso'));
  ExtractTemporaryFile('extract-xiso.exe');
  ExtractTemporaryFile('narutorise_setup_helper.exe');
  ExtractPage.Caption := Title;
  ExtractPage.Description := Description;
  HelperPhase := Description;
  CancelImportButton.Enabled := True;
  ExtractPage.SetText(Description, '');
  ExtractPage.SetProgress(0, 0);
  ExtractPage.ProgressBar.Style := npbstMarquee;
  ExtractPage.Show;
  HelperRunning := True;
  try
    Result := ExecAndLogOutput(ExpandConstant('{tmp}\narutorise_setup_helper.exe'),
      Params, ExpandConstant('{tmp}'), SW_SHOWNORMAL, ewWaitUntilTerminated,
      ExitCode, @HelperOutput);
    Result := Result and (ExitCode = 0) and not CancelRequested;
    if ExitCode = 2 then CancelRequested := True;
  finally
    HelperRunning := False;
    ExtractPage.Hide;
  end;
end;

function ImportError: String;
begin
  if CancelRequested then Result := CustomMessage('ImportCanceled')
  else if HelperError = 'WRONG_GAME' then Result := CustomMessage('WrongGame')
  else if HelperError = 'INVALID_ISO' then Result := CustomMessage('InvalidIso')
  else Result := CustomMessage('ExtractFailed');
end;

function CheckIso: Boolean;
begin
  IsoBytes := 0;
  IsoFiles := 0;
  Result := RunHelper('--check "' + IsoPage.Values[0] + '" "' +
    ExpandConstant('{tmp}\cancel-iso') + '"', CustomMessage('CheckTitle'),
    CustomMessage('CheckDescription'));
  Result := Result and (IsoBytes > 0) and (IsoFiles > 0);
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  if HelperRunning then
  begin
    CancelImport(WizardForm);
    Cancel := False;
    Confirm := False;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  { Silent installs validate in PrepareToInstall, which returns an error
    without leaving automation waiting at an interactive wizard page. }
  if WizardSilent then Exit;
  if (CurPageID = IsoPage.ID) and (IsoPage.Values[0] <> '') then
  begin
    if not FileExists(IsoPage.Values[0]) then
    begin
      MsgBox(CustomMessage('IsoMissing'), mbError, MB_OK);
      Result := False;
    end
    else if GameRootIsOccupied then
    begin
      MsgBox(CustomMessage('IsoConflict'), mbError, MB_OK);
      Result := False;
    end
    else if not CheckIso then
    begin
      MsgBox(ImportError, mbError, MB_OK);
      Result := False;
    end;
  end;
  if (CurPageID = IsoPage.ID) and (IsoPage.Values[0] = '') then
  begin
    IsoBytes := 0;
    IsoFiles := 0;
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo,
  MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  IsoSummary: String;
begin
  IsoSummary := IsoPage.Values[0];
  if IsoSummary = '' then IsoSummary := CustomMessage('IsoSkipped');
  Result := MemoDirInfo + NewLine + NewLine + CustomMessage('IsoLabel') +
    NewLine + Space + IsoSummary;
  if MemoTasksInfo <> '' then Result := Result + NewLine + NewLine + MemoTasksInfo;
  if IsoBytes > 0 then Result := Result + NewLine + NewLine +
    FmtMessage(CustomMessage('SpaceSummary'), [IntToStr((IsoBytes + 1048575) div 1048576)]);
  Result := Result + NewLine + NewLine + CustomMessage('Preserved');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ExtractDir: String;
  DestPath: String;
  TempFree, TempTotal, DestFree, DestTotal, Required: Int64;
begin
  Result := '';
  Extracted := False;
  HadConfig := FileExists(ExpandConstant('{app}\narutorise.toml'));
  if IsoPage.Values[0] = '' then Exit;
  if GameRootIsOccupied then
  begin
    Result := CustomMessage('IsoConflict');
    Exit;
  end;
  if not CheckIso then
  begin
    Result := ImportError;
    Exit;
  end;
  DestPath := ExpandConstant('{app}');
  while (not DirExists(DestPath)) and (ExtractFileDir(DestPath) <> DestPath) do
    DestPath := ExtractFileDir(DestPath);
  { Conservative requirement on BOTH drives also covers a shared volume:
    two copies of game data, port files and a 256 MB safety margin. }
  Required := IsoBytes * 2 + {#PortBytes} + 268435456;
  if (not GetSpaceOnDisk64(ExpandConstant('{tmp}'), TempFree, TempTotal)) or
     (not GetSpaceOnDisk64(DestPath, DestFree, DestTotal)) or
     (TempFree < Required) or (DestFree < Required) then
  begin
    Result := CustomMessage('SpaceError');
    Exit;
  end;
  ExtractDir := ExpandConstant('{tmp}\narutorise_iso');
  DelTree(ExtractDir, True, True, True);
  ExtractPage.ProgressBar.Style := npbstNormal;
  Extracted := RunHelper('--extract "' + IsoPage.Values[0] + '" "' + ExtractDir +
    '" "' + ExpandConstant('{tmp}\cancel-iso') + '" ' + IntToStr(IsoFiles),
    CustomMessage('ExtractTitle'), CustomMessage('ExtractDescription'));
  if not Extracted then
  begin
    DelTree(ExtractDir, True, True, True);
    Result := ImportError;
  end;
end;
