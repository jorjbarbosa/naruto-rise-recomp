#pragma once

namespace localization_strings {

inline constexpr const char* kIt[] = {
    "Naruto: Rise of a Ninja",  // WindowTitle
    "NARUTO: RISE OF A NINJA",  // AppTitle
    "Versione ricompilata per PC (ReXGlue)",  // AppSubtitle
    "GIOCA",  // TabPlay
    "OPZIONI",  // TabSettings
    "FILE DI GIOCO",  // TabGameFiles
    "Qualità grafica",  // GroupGraphics
    "Controlli",  // GroupControls
    "Schermo:",  // DisplayLabel
    "Formato schermo:",  // AspectLabel
    "16:9 - Standard (originale)",  // Aspect16x9
    "21:9 - UltraWide (3440x1440)",  // Aspect21x9
    "32:9 - Super UltraWide (5120x1440)",  // Aspect32x9
    "Regola il campo visivo orizzontale della telecamera 3D senza allungare o deformare i personaggi.",  // AspectTooltip
    "Risoluzione:",  // ResolutionLabel
    "1280x720 - 1x (originale)",  // Res1x
    "2560x1440 - 2x (consigliata)",  // Res2x
    "3840x2160 - 3x (4K UHD)",  // Res3x
    "5120x2880 - 4x (5K UHD)",  // Res4x
    "Renderer:",  // RendererLabel
    "Filtro texture:",  // TextureFilteringLabel
    "Predefinito",  // AnisoDefault
    "Antialiasing:",  // AntiAliasingLabel
    "Disattivato",  // AaOff
    "FXAA",  // AaFxaa
    "FXAA estremo",  // AaFxaaExtreme
    "Post-processing:",  // PostProcessingLabel
    "Bilineare (predefinito)",  // PostBilinear
    "CAS (nitidezza)",  // PostCas
    "FSR EASU (ridimensionamento)",  // PostFsr
    "Dithering",  // DitherLabel
    "Indicatore FPS",  // FpsOverlayLabel
    "Schermo intero",  // FullscreenLabel
    "VSync (limite a 60 Hz)",  // VsyncLabel
    "Lascia attiva questa opzione per mantenere la velocità corretta delle animazioni e della fisica.",  // VsyncTooltip
    "Usa SDL per i controller (Xbox / PlayStation)",  // SdlInputLabel
    "Controller:",  // ControllerStatusLabel
    "Nessun controller rilevato (tastiera e mouse)",  // NoControllerDetected
    "Salta i video introduttivi (loghi Ubisoft)",  // SkipIntroLabel
    "Lingua:",  // LanguageLabel
    "GIOCA",  // BtnPlay
    "Opzioni consigliate",  // BtnResetRecommended
    "Salva opzioni",  // BtnSaveSettings
    "Apri cartella del gioco",  // BtnOpenFolder
    "Le opzioni vengono applicate all'avvio del gioco.",  // SettingsAppliedNotice
    "Opzioni salvate.",  // SettingsSavedNotice
    "Gioco pronto",  // StatusReady
    "File di gioco non trovati",  // StatusNotFound
    "File di gioco trovati in: %s",  // FilesFoundIn
    "Inserisci i file estratti da Xbox 360 in 'game' oppure seleziona la cartella in FILE DI GIOCO. L'estrazione ISO è disponibile nell'installer.",  // FilesMissingDesc
    "Servono i file originali estratti del gioco Xbox 360. Estrai l'ISO con l'installer oppure copia i file in 'game'.",  // FilesNotice
    "Percorso attuale: %s",  // CurrentPathLabel
    "(Nessuna cartella trovata)",  // NoneDetected
    "Seleziona cartella del gioco estratto...",  // BtnSelectFolder
    "Ripristina percorso predefinito",  // BtnResetDefault
    "Percorso predefinito ripristinato.",  // PathResetMsg
    "Le opzioni vengono salvate all'avvio del gioco o alla chiusura del launcher.",  // FooterNotice
    "Gioco avviato!",  // LaunchSuccessMsg
    "Impossibile avviare il processo: ",  // LaunchFailedMsg
    "Eseguibile del gioco non trovato in: ",  // ExeNotFoundMsg
    "Percorso aggiornato: ",  // PathUpdatedMsg
    "Il tuo viaggio inizia qui.",  // PlayHeading
    "Torna al Villaggio della Foglia.",  // PlayIntro
    "File di gioco rilevati. Puoi giocare.",  // ReadyDetail
    "Gestisci i file di gioco",  // ManageFiles
    "Adatta il gioco al tuo PC.",  // SettingsIntro
    "Collega i tuoi file originali alla versione per PC.",  // FilesIntro
    "Cartella del gioco",  // FolderHeading
    "Seleziona la cartella contenente default.xex. I file originali non vengono spostati né eliminati.",  // FolderHint
    "Hai un'ISO?",  // SetupHeading
    "Esegui di nuovo l'installer per estrarre la tua ISO originale di Xbox 360. L'importazione ISO è disponibile nell'installer.",  // SetupHint
    "Opzioni consigliate applicate.",  // RecommendedApplied
    "Impossibile salvare. Controlla i permessi di scrittura della cartella.",  // SaveFailed
    "Manca l'eseguibile del gioco per PC. Reinstalla la versione per PC.",  // ExeMissing
    "Versione per PC non trovata",  // ExeMissingStatus
    "File di gioco rilevati",  // FilesDetected
    "Chiudi launcher",  // Exit
    "DLC",  // TabDlc
    "Contenuti scaricabili",  // DlcHeading
    "Installa i pacchetti di personaggi DLC per Naruto: Rise of a Ninja.",  // DlcIntro
    "DLC installati",  // DlcInstalledHeading
    "Nessun DLC installato.",  // DlcNoInstalled
    "Installa cartella DLC",  // DlcInstallLoose
    "Seleziona una cartella DLC. I pacchetti Xbox 360 (.live/.con/.pirs) vengono installati all'avvio del gioco; le cartelle già estratte vengono copiate direttamente.",  // DlcInstallLooseHint
    "Seleziona cartella DLC...",  // DlcSelectLooseFolder
    "DLC installato.",  // DlcInstallSuccess
    "Installazione DLC non riuscita:",  // DlcInstallFailed
    "Pacchetti DLC pronti. Verranno installati al prossimo avvio del gioco.",  // DlcStfsStaged
    "Apri cartella DLC",  // DlcOpenContentFolder
    "Prepara cartelle DLC",  // DlcPrepareFolders
    "Le cartelle DLC sono pronte.",  // DlcFoldersReady
    "Naruto: Rise of a Ninja - Installazione",  // InstWindowTitle
    "Benvenuto nell'installer di Naruto: Rise of a Ninja per PC",  // InstWelcomeTitle
    "Questa procedura installa la versione per PC e importa i tuoi file originali del gioco Xbox 360.",  // InstWelcomeIntro
    "1. Scegli la cartella di installazione",  // InstWelcomeStep1
    "2. Seleziona la tua ISO di Naruto: Rise of a Ninja",  // InstWelcomeStep2
    "3. I file di gioco vengono estratti automaticamente",  // InstWelcomeStep3
    "4. Vengono creati i collegamenti (facoltativo)",  // InstWelcomeStep4
    "5. Gioca tramite il launcher",  // InstWelcomeStep5
    "Devi possedere il gioco originale. Questa versione non distribuisce file di gioco.",  // InstWelcomeLegal
    "File di installazione non trovati. Esegui questo installer dalla sua cartella di distribuzione oppure usa l'installer generato.",  // InstPayloadMissing
    "Cartella di installazione",  // InstDestTitle
    "I file della versione per PC verranno copiati in questa cartella.",  // InstDestHint
    "Sfoglia...",  // InstBrowse
    "Avanti",  // InstNext
    "Indietro",  // InstBack
    "INSTALLA",  // InstInstall
    "Fine",  // InstFinish
    "Immagine ISO del gioco",  // InstIsoTitle
    "Seleziona la tua ISO originale di Xbox 360 (.iso).",  // InstIsoHint
    "Seleziona l'ISO di Naruto: Rise of a Ninja",  // InstIsoDialog
    "Verifica dell'ISO...",  // InstIsoChecking
    "ISO Xbox 360 valida: %u file (%.2f GB)",  // InstIsoValid
    "Il file selezionato non è un'ISO Xbox 360 valida.",  // InstIsoInvalid
    "default.xex non è stato trovato nell'ISO.",  // InstIsoNoXex
    "Salta estrazione (i file di gioco sono già nella cartella di destinazione)",  // InstIsoSkipCheck
    "Seleziona un'ISO valida oppure attiva 'Salta estrazione' se i file di gioco sono già presenti.",  // InstIsoRequired
    "Inserisci una cartella di installazione valida.",  // InstInvalidDest
    "Installazione",  // InstProgressTitle
    "Preparazione dell'installazione...",  // InstPreparing
    "Rimozione dei file di gioco precedenti...",  // InstRemovingOld
    "Estrazione dell'ISO (può richiedere alcuni minuti)...",  // InstExtractingIso
    "Copia dei file della versione per PC...",  // InstCopyingFiles
    "Scrittura della configurazione...",  // InstWritingConfig
    "Registrazione nell'elenco dei programmi...",  // InstRegistering
    "Installazione completata!",  // InstComplete
    "Installazione annullata.",  // InstCanceled
    "Installazione non riuscita",  // InstFailedTitle
    "Installazione completata",  // InstFinishTitle
    "Installato in: %s",  // InstInstalledTo
    "File di gioco in: %s",  // InstGameFilesAt
    "Crea collegamento sul desktop",  // InstShortcutDesktop
    "Crea collegamento nel menu Start",  // InstShortcutStartMenu
    "Avvia ora il launcher di Naruto",  // InstLaunchNow
    "Naruto: Rise of a Ninja - Disinstallazione",  // UnWindowTitle
    "Rimuove i file della versione per PC, i collegamenti e la voce nell'elenco dei programmi.",  // UnIntro
    "Disinstalla (conserva i file di gioco)",  // UnKeepFiles
    "Disinstalla tutto",  // UnRemoveAll
    "Annulla",  // UnCancel
    "Disinstallazione completata. Alcuni file potrebbero restare fino alla chiusura di questa finestra.",  // UnDone
    "Disinstallazione non riuscita: %s",  // UnFailed
    "Gioco",  // GroupGame
    "Lingua del gioco:",  // GameLanguageLabel
    "Inglese",  // GameLanguageEnglish
    "Francese",  // GameLanguageFrench
    "Tedesco",  // GameLanguageGerman
    "Spagnolo",  // GameLanguageSpanish
    "Italiano",  // GameLanguageItalian
    "Personalizzata (%u)",  // GameLanguageCustom
    "Applicata all'avvio del gioco. I testi e le voci disponibili dipendono dai file di gioco.",  // GameLanguageHint
    "Lingua dell'interfaccia del launcher",  // LauncherLanguageTooltip
};

}  // namespace localization_strings
