#pragma once

namespace localization_strings {

inline constexpr const char* kIt[] = {
    "Naruto: Rise of a Ninja",  // WindowTitle
    "GIOCA",  // TabPlay
    "OPZIONI",  // TabSettings
    "FILE DI GIOCO",  // TabGameFiles
    "Qualità",  // GroupQuality
    "Controlli",  // GroupControls
    "Formato schermo",  // AspectLabel
    "16:9 - Standard (originale)",  // Aspect16x9
    "21:9 - UltraWide",  // Aspect21x9
    "32:9 - Super UltraWide",  // Aspect32x9
    "Regola il campo visivo orizzontale della telecamera 3D senza allungare o deformare i personaggi.",  // AspectDescription
    "Risoluzione",  // ResolutionLabel
    "Renderer",  // RendererLabel
    "Filtro texture",  // TextureFilteringLabel
    "Predefinito",  // AnisoDefault
    "FXAA",  // FxaaLabel
    "Disattivato",  // FxaaOff
    "Normale",  // FxaaNormal
    "Estremo",  // FxaaExtreme
    "Post-processing",  // PostProcessingLabel
    "Bilineare (predefinito)",  // PostBilinear
    "CAS (nitidezza)",  // PostCas
    "FSR EASU (ridimensionamento)",  // PostFsr
    "Dithering",  // DitherLabel
    "Indicatore FPS",  // FpsOverlayLabel
    "Schermo intero",  // FullscreenLabel
    "VSync",  // VsyncLabel
    "Lascia attiva questa opzione per mantenere la velocità corretta delle animazioni e della fisica.",  // VsyncDescription
    "Usa SDL per i controller (Xbox / PlayStation)",  // SdlInputLabel
    "Controller",  // ControllerLabel
    "Nessun controller rilevato (tastiera e mouse)",  // NoControllerDetected
    "Salta i video introduttivi (loghi Ubisoft)",  // SkipIntroLabel
    "GIOCA",  // BtnPlay
    "Opzioni consigliate",  // BtnResetRecommended
    "Apri cartella del gioco",  // BtnOpenFolder
    "Le opzioni vengono applicate all'avvio del gioco.",  // SettingsAppliedNotice
    "Gioco pronto",  // StatusReady
    "File di gioco non trovati",  // StatusNotFound
    "Inserisci i file estratti da Xbox 360 in 'game' oppure seleziona la cartella in FILE DI GIOCO. L'estrazione ISO è disponibile nell'installer.",  // FilesMissingDesc
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
    "Collega i tuoi file originali alla versione per PC.",  // FilesIntro
    "Cartella del gioco",  // FolderHeading
    "Seleziona la cartella contenente default.xex. I file originali non vengono spostati né eliminati.",  // FolderHint
    "Hai un'ISO?",  // SetupHeading
    "Esegui di nuovo l'installer per estrarre la tua ISO originale di Xbox 360. L'importazione ISO è disponibile nell'installer.",  // SetupHint
    "Opzioni consigliate applicate.",  // RecommendedApplied
    "Manca l'eseguibile del gioco per PC. Reinstalla la versione per PC.",  // ExeMissing
    "Versione per PC non trovata",  // ExeMissingStatus
    "File di gioco rilevati",  // FilesDetected
    "DLC",  // TabDlc
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
    "Lingua del gioco",  // GameLanguageLabel
    "Inglese",  // GameLanguageEnglish
    "Francese",  // GameLanguageFrench
    "Tedesco",  // GameLanguageGerman
    "Spagnolo",  // GameLanguageSpanish
    "Italiano",  // GameLanguageItalian
    "Personalizzata (%u)",  // GameLanguageCustom
    "Applicata all'avvio del gioco. Le traduzioni installate in mods appaiono qui e sostituiscono i testi solo quando selezionate. Le voci disponibili dipendono dai file di gioco.",  // GameLanguageDescription
    "Lingua dell'interfaccia del launcher",  // LauncherLanguageTooltip
    "Xenos (GPU emulata)",  // RendererXenos
    "Nativo",  // RendererNative
    "API grafica usata dal gioco: Direct3D 12 o Vulkan. La scelta si applica al prossimo avvio.",  // RendererDescription
    "Non disponibile con il renderer nativo. Scegli Xenos in Renderer per usarlo.",  // XenosOnlyReason
    "MSAA",  // MsaaLabel
    "4x (originale)",  // MsaaOriginal
    "Multicampionamento delle scene 3D del gioco. 8x richiede una GPU più potente.",  // MsaaDescription
    "AA della trasparenza",  // TransparencyAaLabel
    "Ammorbidisce i bordi di fogliame, recinzioni e altre texture ritagliate (alpha to coverage).",  // TransparencyAaDescription
    "Frequenza fotogrammi",  // FrameRateLabel
    "30 è la frequenza originale del gioco. Oltre, la logica mantiene il suo ritmo e i fotogrammi intermedi vengono interpolati. Con il VSync attivo si ferma a quella del monitor; le frequenze alte richiedono una CPU più veloce.",  // FrameRateDescription
    "Luminosità",  // BrightnessLabel
    "50 è la luminosità originale del gioco.",  // BrightnessDescription
    "Latenza dei fotogrammi",  // FrameLatencyLabel
    "1 (minima)",  // FrameLatencyLowest
    "Fotogrammi in coda prima della visualizzazione. 1 ha il ritardo di input più basso; 2 o 3 possono uniformare tempi dei fotogrammi irregolari.",  // FrameLatencyDescription
    "Triplo buffer",  // TripleBufferingLabel
    "Tre buffer di visualizzazione invece di due.",  // TripleBufferingDescription
    "Sincronizza la presentazione con lo schermo. Con il renderer nativo, la velocità del gioco non dipende dal VSync.",  // VsyncNativeDescription
    "Scheda video",  // GraphicsDeviceLabel
    "Automatica",  // GraphicsDeviceAuto
    "La scheda video su cui gira il gioco. Automatica lascia la scelta al renderer. Sui portatili con due schede video, scegli quella dedicata (NVIDIA o AMD).",  // GraphicsDeviceDescription
    "Schermo",  // GroupDisplay
    "Avanzate",  // GroupAdvanced
    "solo Xenos",  // XenosOnlyTag
    "solo nativo",  // NativeOnlyTag
    "Solo con il renderer nativo. Scegli Nativo in Renderer per usarlo.",  // NativeOnlyReason
    "La risoluzione a cui viene disegnato il gioco, in multipli dei 1280×720 originali. Valori più alti sono più nitidi e richiedono una scheda video più potente.",  // ResolutionDescription
    "Il filtro anisotropico mantiene nitide le texture delle superfici viste di sbieco, come pavimenti e strade.",  // TextureFilteringDescription
    "Ammorbidisce i bordi seghettati filtrando l'immagine finale. Estremo leviga di più e può sfocare i dettagli fini.",  // FxaaDescription
    "Come l'immagine viene adattata allo schermo. CAS aumenta la nitidezza, FSR la ingrandisce con nitidezza e Bilineare la lascia morbida.",  // PostProcessingDescription
    "Aggiunge un rumore fine che nasconde le bande di colore nelle sfumature.",  // DitherDescription
    "Mostra frequenza e tempo dei fotogrammi in un angolo dello schermo.",  // FpsOverlayDescription
    "Avvia il gioco a schermo intero. Disattivato, gira in una finestra.",  // FullscreenDescription
    "Salta i loghi mostrati all'avvio del gioco. Il filmato d'apertura resta.",  // SkipIntroDescription
    "Legge i controller tramite SDL, che riconosce controller Xbox, PlayStation e altri. Disattivato, il gioco usa XInput, che riconosce solo controller compatibili con Xbox.",  // SdlInputDescription
    "Il controller collegato ora, con il nome che gli dà SDL.",  // ControllerDescription
    "Usa tastiera e mouse",  // KeyboardMouseLabel
    "Tastiera e mouse sostituiscono un controller Xbox e funzionano insieme a un controller collegato. Con il mouse sulla telecamera, il cursore resta nella finestra del gioco finché è attiva; Alt+Tab lo libera. Il gioco continua a mostrare i pulsanti del controller.",  // KeyboardMouseDescription
    "Il mouse muove la telecamera",  // MouseCameraLabel
    "Muove la telecamera (la levetta destra) con il mouse. Disattivato, la telecamera si muove solo con i tasti della levetta destra.",  // MouseCameraDescription
    "Sensibilità",  // MouseSensitivityLabel
    "La velocità con cui la telecamera segue il mouse. 100% è il valore predefinito.",  // MouseSensitivityDescription
    "Tasti",  // KeysLabel
    "I tasti e i pulsanti del mouse per ogni comando del controller, due per comando.",  // KeysDescription
    "Assegna tasti…",  // MapKeysButton
    "Assegna tasti",  // MapKeysTitle
    "Controlli › Tastiera e mouse",  // MapKeysPath
    "Pulsanti",  // KeysButtonsHeading
    "Levetta sinistra",  // LeftStickName
    "Levetta destra",  // RightStickName
    "Croce direzionale",  // DpadName
    "+ mouse",  // KeysPlusMouse
    "Su",  // DirectionUp
    "Giù",  // DirectionDown
    "Sinistra",  // DirectionLeft
    "Destra",  // DirectionRight
    "Fai clic e premi il nuovo tasto (5 s). Il clic destro cancella. Un tasto già usato lascia il comando precedente.",  // KeysHint
    "Premi… %d",  // KeyCapturePrompt
    "Ripristina tasti predefiniti",  // KeysRestoreDefaults
    "Fatto",  // KeysDone
    "%s è stato tolto da %s.",  // KeyMovedStatus
    "%s è usato dal gioco e non può essere assegnato.",  // KeyReservedStatus
    "Tasti predefiniti ripristinati.",  // KeysRestoredStatus
    "Spazio",  // KeySpaceName
    "Mouse sx",  // MouseLeftName
    "Mouse dx",  // MouseRightName
    "Mouse centrale",  // MouseMiddleName
    "API grafica nativa",  // NativeBackendLabel
    "Scegli Direct3D 12 o Vulkan. Richiede il riavvio del gioco. Vulkan mantiene una presentazione in attesa.",  // NativeBackendDescription
    "Controlla aggiornamenti",  // UpdatesCheck
    "Ricerca aggiornamenti…",  // UpdatesChecking
    "La tua versione è aggiornata.",  // UpdatesCurrent
    "Verifica non riuscita. Riprova più tardi.",  // UpdatesFailed
    "Impossibile confrontare la versione installata.",  // UpdatesUnknownVersion
    "Nuova versione disponibile: %s",  // UpdatesAvailable
    "Novità",  // UpdatesNotes
    "Scarica aggiornamento",  // UpdatesDownload
    "Programma di installazione Windows",  // UpdatesInstaller
    "ZIP portatile",  // UpdatesPortable
    "Più tardi",  // UpdatesLater
    "Ignora questa versione",  // UpdatesIgnore
    "Impossibile aprire il browser. Riprova.",  // UpdatesBrowserFailed
    "Apre il download nel browser. Chiudi il gioco e il launcher prima di applicare l’aggiornamento.",  // UpdatesDownloadHint
    "Attendi. È consentita una verifica al minuto.",  // UpdatesCheckCooldown
    "Download manuale",  // UpdatesManualDownload
    "Download dell’aggiornamento…",  // UpdatesDownloading
    "Verifica del download…",  // UpdatesVerifying
    "Annullamento…",  // UpdatesCancelling
    "Annulla",  // UpdatesCancel
    "Download annullato.",  // UpdatesCancelled
    "Download verificato. Il launcher si chiuderà e il programma di installazione aggiornerà questa cartella:",  // UpdatesReadyToInstall
    "Installa e chiudi il launcher",  // UpdatesInstallClose
    "Download non riuscito. Verifica la connessione e riprova.",  // UpdatesDownloadFailed
    "Il file non ha superato la verifica. Scaricalo di nuovo.",  // UpdatesIntegrityFailed
    "Impossibile confermare l’aggiornamento. Riprova o apri la pagina della versione da Novità.",  // UpdatesMetadataFailed
    "Impossibile salvare il download. Verifica lo spazio libero e i permessi della cartella.",  // UpdatesStorageFailed
    "Chiudi il gioco e gli altri launcher che usano questa cartella, poi riprova a installare.",  // UpdatesCloseGame
    "Impossibile avviare il programma di installazione. Riprova o apri la pagina della versione da Novità.",  // UpdatesInstallFailed
    "Impossibile salvare le impostazioni. Correggi i permessi del file prima di installare.",  // UpdatesSaveFailed
    "La registrazione dell’installazione è cambiata. Chiudi e riapri il launcher, poi riprova.",  // UpdatesNotInstalled
    "Scarica e installa",  // UpdatesDownloadInstall
    "Avvia programma di installazione",  // UpdatesLaunchInstaller
    "Il programma di installazione verrà scaricato e verificato. Poi chiederemo un’altra conferma prima di chiudere il launcher e aprire la procedura guidata per aggiornare il gioco.",  // UpdatesConfirmDownload
    "Il programma di installazione è stato scaricato e verificato. Vuoi avviarlo ora? Le impostazioni verranno salvate e il launcher si chiuderà. La procedura guidata ti accompagnerà nell’aggiornamento. Chiudi il gioco prima di continuare.",  // UpdatesConfirmInstall
    "Questa copia è portatile. Il programma di installazione la registrerà come applicazione Windows nella stessa cartella. Questo processo non usa lo ZIP.",  // UpdatesPortableConversion
    "Cartella di installazione",  // UpdatesDestination
    "Continuare con il download?",  // UpdatesContinue
    "Sì",  // UpdatesYes
    "No",  // UpdatesNo
    "Download pronto. Premi Scarica e installa per aprire il programma di installazione.",  // UpdatesDownloadReady
    "Questa versione non ha un programma di installazione Windows. Consulta Novità.",  // UpdatesNoInstaller
    "Annulla download",  // UpdatesCancelDownload
    "Questa cartella non contiene il port completo. Usa il launcher nella cartella con narutorise.exe e narutorise_launcher.exe.",  // UpdatesMissingPortFiles
};

}  // namespace localization_strings
