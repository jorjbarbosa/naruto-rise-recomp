#pragma once

namespace localization_strings {

inline constexpr const char* kDe[] = {
    "Naruto: Rise of a Ninja",  // WindowTitle
    "SPIELEN",  // TabPlay
    "OPTIONEN",  // TabSettings
    "SPIELDATEIEN",  // TabGameFiles
    "Qualität",  // GroupQuality
    "Steuerung",  // GroupControls
    "Seitenverhältnis",  // AspectLabel
    "16:9 - Standard (Original)",  // Aspect16x9
    "21:9 - UltraWide",  // Aspect21x9
    "32:9 - Super UltraWide",  // Aspect32x9
    "Passt das horizontale Sichtfeld der 3D-Kamera an, ohne die Figuren zu strecken oder zu verzerren.",  // AspectDescription
    "Auflösung",  // ResolutionLabel
    "Renderer",  // RendererLabel
    "Texturfilter",  // TextureFilteringLabel
    "Standard",  // AnisoDefault
    "FXAA",  // FxaaLabel
    "Aus",  // FxaaOff
    "Normal",  // FxaaNormal
    "Extrem",  // FxaaExtreme
    "Nachbearbeitung",  // PostProcessingLabel
    "Bilinear (Standard)",  // PostBilinear
    "CAS (Schärfen)",  // PostCas
    "FSR EASU (Skalierung)",  // PostFsr
    "Dithering",  // DitherLabel
    "FPS-Anzeige",  // FpsOverlayLabel
    "Vollbild",  // FullscreenLabel
    "VSync",  // VsyncLabel
    "Aktiviert lassen, damit Animationen und Physik mit der richtigen Geschwindigkeit laufen.",  // VsyncDescription
    "SDL für Controller verwenden (Xbox / PlayStation)",  // SdlInputLabel
    "Controller",  // ControllerLabel
    "Kein Controller erkannt (Tastatur und Maus)",  // NoControllerDetected
    "Intro-Videos überspringen (Ubisoft-Logos)",  // SkipIntroLabel
    "SPIEL STARTEN",  // BtnPlay
    "Empfohlene Optionen",  // BtnResetRecommended
    "Spielordner öffnen",  // BtnOpenFolder
    "Die Optionen werden beim Spielstart angewendet.",  // SettingsAppliedNotice
    "Spiel bereit",  // StatusReady
    "Spieldateien nicht gefunden",  // StatusNotFound
    "Lege die extrahierten Xbox-360-Dateien in 'game' ab oder wähle ihren Ordner unter SPIELDATEIEN. ISO-Dateien können im Installationsprogramm extrahiert werden.",  // FilesMissingDesc
    "(Kein Ordner gefunden)",  // NoneDetected
    "Ordner mit extrahierten Spieldateien wählen...",  // BtnSelectFolder
    "Standardpfad verwenden",  // BtnResetDefault
    "Standardpfad wiederhergestellt.",  // PathResetMsg
    "Optionen werden beim Spielstart oder beim Schließen des Launchers gespeichert.",  // FooterNotice
    "Spiel gestartet!",  // LaunchSuccessMsg
    "Prozess konnte nicht gestartet werden: ",  // LaunchFailedMsg
    "Spielprogramm nicht gefunden unter: ",  // ExeNotFoundMsg
    "Pfad aktualisiert: ",  // PathUpdatedMsg
    "Deine Reise beginnt hier.",  // PlayHeading
    "Kehre nach Konoha zurück.",  // PlayIntro
    "Spieldateien erkannt. Du kannst loslegen.",  // ReadyDetail
    "Spieldateien verwalten",  // ManageFiles
    "Verbinde deine Originaldateien mit dem PC-Port.",  // FilesIntro
    "Spielordner",  // FolderHeading
    "Wähle den Ordner mit default.xex. Deine Originaldateien werden weder verschoben noch gelöscht.",  // FolderHint
    "Du hast eine ISO-Datei?",  // SetupHeading
    "Starte das Installationsprogramm erneut, um deine originale Xbox-360-ISO zu extrahieren. ISO-Import ist im Installationsprogramm verfügbar.",  // SetupHint
    "Empfohlene Optionen angewendet.",  // RecommendedApplied
    "Das PC-Spielprogramm fehlt. Installiere den Port erneut.",  // ExeMissing
    "PC-Port nicht gefunden",  // ExeMissingStatus
    "Spieldateien erkannt",  // FilesDetected
    "DLC",  // TabDlc
    "Installiere DLC-Charakterpakete für Naruto: Rise of a Ninja.",  // DlcIntro
    "Installierte DLCs",  // DlcInstalledHeading
    "Noch keine DLCs installiert.",  // DlcNoInstalled
    "DLC-Ordner installieren",  // DlcInstallLoose
    "Wähle einen DLC-Ordner. Xbox-360-Pakete (.live/.con/.pirs) werden beim Spielstart installiert; bereits extrahierte Ordner werden direkt kopiert.",  // DlcInstallLooseHint
    "DLC-Ordner wählen...",  // DlcSelectLooseFolder
    "DLC installiert.",  // DlcInstallSuccess
    "DLC-Installation fehlgeschlagen:",  // DlcInstallFailed
    "DLC-Pakete vorbereitet. Sie werden beim nächsten Spielstart installiert.",  // DlcStfsStaged
    "DLC-Ordner öffnen",  // DlcOpenContentFolder
    "DLC-Ordner vorbereiten",  // DlcPrepareFolders
    "DLC-Ordner sind bereit.",  // DlcFoldersReady
    "Naruto: Rise of a Ninja - Installation",  // InstWindowTitle
    "Willkommen bei der Installation des PC-Ports von Naruto: Rise of a Ninja",  // InstWelcomeTitle
    "Dieser Assistent installiert den PC-Port und importiert deine Originaldateien des Xbox-360-Spiels.",  // InstWelcomeIntro
    "1. Installationsordner wählen",  // InstWelcomeStep1
    "2. ISO-Datei von Naruto: Rise of a Ninja wählen",  // InstWelcomeStep2
    "3. Spieldateien werden automatisch extrahiert",  // InstWelcomeStep3
    "4. Verknüpfungen werden erstellt (optional)",  // InstWelcomeStep4
    "5. Über den Launcher spielen",  // InstWelcomeStep5
    "Du musst das Originalspiel besitzen. Mit diesem Port werden keine Spieldateien verteilt.",  // InstWelcomeLegal
    "Installationsdateien nicht gefunden. Starte dieses Programm aus seinem Distributionsordner oder verwende das erstellte Setup.",  // InstPayloadMissing
    "Installationsordner",  // InstDestTitle
    "Die Dateien des Ports werden in diesen Ordner kopiert.",  // InstDestHint
    "Durchsuchen...",  // InstBrowse
    "Weiter",  // InstNext
    "Zurück",  // InstBack
    "INSTALLIEREN",  // InstInstall
    "Fertig",  // InstFinish
    "ISO-Datei des Spiels",  // InstIsoTitle
    "Wähle deine originale Xbox-360-ISO-Datei (.iso).",  // InstIsoHint
    "ISO von Naruto: Rise of a Ninja wählen",  // InstIsoDialog
    "ISO-Datei wird geprüft...",  // InstIsoChecking
    "Gültige Xbox-360-ISO: %u Dateien (%.2f GB)",  // InstIsoValid
    "Die gewählte Datei ist keine gültige Xbox-360-ISO.",  // InstIsoInvalid
    "default.xex wurde in der ISO nicht gefunden.",  // InstIsoNoXex
    "Extraktion überspringen (Spieldateien sind bereits im Zielordner)",  // InstIsoSkipCheck
    "Wähle eine gültige ISO oder aktiviere 'Extraktion überspringen', wenn die Spieldateien bereits vorhanden sind.",  // InstIsoRequired
    "Gib einen gültigen Installationsordner an.",  // InstInvalidDest
    "Installation",  // InstProgressTitle
    "Installation wird vorbereitet...",  // InstPreparing
    "Vorherige Spieldateien werden entfernt...",  // InstRemovingOld
    "ISO wird extrahiert (dies kann einige Minuten dauern)...",  // InstExtractingIso
    "Dateien des Ports werden kopiert...",  // InstCopyingFiles
    "Konfiguration wird gespeichert...",  // InstWritingConfig
    "Eintrag in der Programmliste wird erstellt...",  // InstRegistering
    "Installation abgeschlossen!",  // InstComplete
    "Installation abgebrochen.",  // InstCanceled
    "Installation fehlgeschlagen",  // InstFailedTitle
    "Installation abgeschlossen",  // InstFinishTitle
    "Installiert in: %s",  // InstInstalledTo
    "Spieldateien in: %s",  // InstGameFilesAt
    "Desktop-Verknüpfung erstellen",  // InstShortcutDesktop
    "Startmenü-Verknüpfung erstellen",  // InstShortcutStartMenu
    "Naruto-Launcher jetzt starten",  // InstLaunchNow
    "Naruto: Rise of a Ninja - Deinstallation",  // UnWindowTitle
    "Entfernt die Dateien des Ports, die Verknüpfungen und den Eintrag in der Programmliste.",  // UnIntro
    "Deinstallieren (Spieldateien behalten)",  // UnKeepFiles
    "Alles deinstallieren",  // UnRemoveAll
    "Abbrechen",  // UnCancel
    "Deinstallation abgeschlossen. Einige Dateien bleiben möglicherweise bis zum Schließen dieses Fensters erhalten.",  // UnDone
    "Deinstallation fehlgeschlagen: %s",  // UnFailed
    "Spiel",  // GroupGame
    "Spielsprache",  // GameLanguageLabel
    "Englisch",  // GameLanguageEnglish
    "Französisch",  // GameLanguageFrench
    "Deutsch",  // GameLanguageGerman
    "Spanisch",  // GameLanguageSpanish
    "Italienisch",  // GameLanguageItalian
    "Benutzerdefiniert (%u)",  // GameLanguageCustom
    "Wird beim nächsten Spielstart angewendet. Übersetzungen im Ordner mods erscheinen hier und ersetzen Texte nur, wenn sie ausgewählt sind. Verfügbare Stimmen hängen von deinen Spieldateien ab.",  // GameLanguageDescription
    "Sprache der Launcher-Oberfläche",  // LauncherLanguageTooltip
    "Xenos (emulierte GPU)",  // RendererXenos
    "Nativ",  // RendererNative
    "Grafik-API des Spiels: Direct3D 12 oder Vulkan. Die Auswahl gilt ab dem nächsten Spielstart.",  // RendererDescription
    "Mit dem nativen Renderer nicht verfügbar. Unter Renderer Xenos wählen, um es zu nutzen.",  // XenosOnlyReason
    "MSAA",  // MsaaLabel
    "4x (Original)",  // MsaaOriginal
    "Multisampling der 3D-Szenen des Spiels. 8x braucht eine stärkere GPU.",  // MsaaDescription
    "Transparenz-AA",  // TransparencyAaLabel
    "Glättet die Kanten von Laub, Zäunen und anderen ausgeschnittenen Texturen (Alpha to Coverage).",  // TransparencyAaDescription
    "Bildrate",  // FrameRateLabel
    "30 ist die Originalbildrate. Darüber behält die Spiellogik ihr Tempo, und die Zwischenbilder werden interpoliert. Mit VSync endet sie bei der Rate des Monitors; hohe Bildraten brauchen eine schnellere CPU.",  // FrameRateDescription
    "Helligkeit",  // BrightnessLabel
    "50 ist die Originalhelligkeit des Spiels.",  // BrightnessDescription
    "Bildlatenz",  // FrameLatencyLabel
    "1 (minimal)",  // FrameLatencyLowest
    "Bilder in der Warteschlange vor der Anzeige. 1 hat die geringste Eingabeverzögerung; 2 oder 3 können ungleichmäßige Bildzeiten glätten.",  // FrameLatencyDescription
    "Dreifachpufferung",  // TripleBufferingLabel
    "Drei Anzeigepuffer statt zwei.",  // TripleBufferingDescription
    "Synchronisiert die Ausgabe mit dem Bildschirm. Mit dem nativen Renderer hängt die Spielgeschwindigkeit nicht von VSync ab.",  // VsyncNativeDescription
    "Grafikkarte",  // GraphicsDeviceLabel
    "Automatisch",  // GraphicsDeviceAuto
    "Die Grafikkarte, auf der das Spiel läuft. Automatisch überlässt die Wahl dem Renderer. Auf Laptops mit zwei Grafikkarten die dedizierte wählen (NVIDIA oder AMD).",  // GraphicsDeviceDescription
    "Anzeige",  // GroupDisplay
    "Erweitert",  // GroupAdvanced
    "nur Xenos",  // XenosOnlyTag
    "nur nativ",  // NativeOnlyTag
    "Nur mit dem nativen Renderer. Unter Renderer Nativ wählen, um es zu nutzen.",  // NativeOnlyReason
    "Die Auflösung, in der das Spiel gezeichnet wird, als Vielfaches der originalen 1280×720. Höhere Werte sehen schärfer aus und brauchen eine stärkere Grafikkarte.",  // ResolutionDescription
    "Anisotrope Filterung hält Texturen auf schräg gesehenen Flächen wie Böden und Straßen scharf.",  // TextureFilteringDescription
    "Glättet gezackte Kanten, indem das fertige Bild gefiltert wird. Extrem glättet stärker und kann feine Details verwischen.",  // FxaaDescription
    "Wie das Bild an den Bildschirm angepasst wird. CAS schärft es, FSR skaliert es mit Schärfung hoch, und Bilinear lässt es weich.",  // PostProcessingDescription
    "Fügt feines Rauschen hinzu, das Farbstufen in Verläufen verbirgt.",  // DitherDescription
    "Zeigt Bildrate und Bildzeit in einer Bildschirmecke. F1 schaltet die Anzeige im Spiel um.",  // FpsOverlayDescription
    "Startet das Spiel im Vollbild. Ausgeschaltet läuft es in einem Fenster.",  // FullscreenDescription
    "Überspringt die Logos beim Spielstart. Das Eröffnungsvideo bleibt erhalten.",  // SkipIntroDescription
    "Liest Controller über SDL, das Xbox-, PlayStation- und andere Controller erkennt. Ausgeschaltet nutzt das Spiel XInput, das nur Xbox-kompatible Controller erkennt.",  // SdlInputDescription
    "Der gerade verbundene Controller, wie SDL ihn nennt.",  // ControllerDescription
    "Tastatur und Maus verwenden",  // KeyboardMouseLabel
    "Tastatur und Maus ersetzen einen Xbox-Controller und funktionieren zusammen mit einem angeschlossenen Controller. Steuert die Maus die Kamera, bleibt der Mauszeiger im Spielfenster, solange es aktiv ist; Alt+Tab gibt ihn frei. Das Spiel zeigt weiterhin die Controller-Tasten an.",  // KeyboardMouseDescription
    "Maus steuert die Kamera",  // MouseCameraLabel
    "Bewegt die Kamera (den rechten Stick) mit der Maus. Aus: Die Kamera bewegt sich nur mit den Tasten des rechten Sticks.",  // MouseCameraDescription
    "Empfindlichkeit",  // MouseSensitivityLabel
    "Wie schnell die Kamera der Maus folgt. 100 % ist der Standard.",  // MouseSensitivityDescription
    "Tasten",  // KeysLabel
    "Die Tasten und Maustasten für jede Controller-Eingabe, zwei pro Eingabe.",  // KeysDescription
    "Tasten belegen…",  // MapKeysButton
    "Tasten belegen",  // MapKeysTitle
    "Steuerung › Tastatur und Maus",  // MapKeysPath
    "Knöpfe",  // KeysButtonsHeading
    "Linker Stick",  // LeftStickName
    "Rechter Stick",  // RightStickName
    "Steuerkreuz",  // DpadName
    "+ Maus",  // KeysPlusMouse
    "Oben",  // DirectionUp
    "Unten",  // DirectionDown
    "Links",  // DirectionLeft
    "Rechts",  // DirectionRight
    "Klicken und die neue Taste drücken (5 s). Rechtsklick löscht. Eine bereits belegte Taste verlässt ihre bisherige Eingabe.",  // KeysHint
    "Taste… %d",  // KeyCapturePrompt
    "Standardtasten wiederherstellen",  // KeysRestoreDefaults
    "Fertig",  // KeysDone
    "%s wurde von %s entfernt.",  // KeyMovedStatus
    "%s wird vom Spiel verwendet und kann nicht belegt werden.",  // KeyReservedStatus
    "Standardtasten wiederhergestellt.",  // KeysRestoredStatus
    "Leertaste",  // KeySpaceName
    "Linke Maus",  // MouseLeftName
    "Rechte Maus",  // MouseRightName
    "Mittlere Maus",  // MouseMiddleName
    "Native Grafik-API",  // NativeBackendLabel
    "Direct3D 12 oder Vulkan wählen. Neustart des Spiels erforderlich. Vulkan begrenzt die Warteschlange auf eine Präsentation.",  // NativeBackendDescription
    "Nach Updates suchen",  // UpdatesCheck
    "Suche nach Updates…",  // UpdatesChecking
    "Deine Version ist aktuell.",  // UpdatesCurrent
    "Prüfung fehlgeschlagen. Versuche es später erneut.",  // UpdatesFailed
    "Die installierte Version konnte nicht verglichen werden.",  // UpdatesUnknownVersion
    "Neue Version verfügbar: %s",  // UpdatesAvailable
    "Neuerungen",  // UpdatesNotes
    "Update herunterladen",  // UpdatesDownload
    "Windows-Installer",  // UpdatesInstaller
    "Portables ZIP",  // UpdatesPortable
    "Später",  // UpdatesLater
    "Diese Version ignorieren",  // UpdatesIgnore
    "Der Browser konnte nicht geöffnet werden. Versuche es erneut.",  // UpdatesBrowserFailed
    "Öffnet den Download im Browser. Schließe das Spiel und den Launcher, bevor du das Update installierst.",  // UpdatesDownloadHint
    "Bitte warten. Es ist nur eine Prüfung pro Minute möglich.",  // UpdatesCheckCooldown
    "Manueller Download",  // UpdatesManualDownload
    "Update wird heruntergeladen…",  // UpdatesDownloading
    "Download wird überprüft…",  // UpdatesVerifying
    "Abbruch läuft…",  // UpdatesCancelling
    "Abbrechen",  // UpdatesCancel
    "Download abgebrochen.",  // UpdatesCancelled
    "Download überprüft. Der Launcher wird geschlossen und Setup aktualisiert diesen Ordner:",  // UpdatesReadyToInstall
    "Installieren und Launcher schließen",  // UpdatesInstallClose
    "Download fehlgeschlagen. Prüfe die Verbindung und versuche es erneut.",  // UpdatesDownloadFailed
    "Die Datei hat die Prüfung nicht bestanden. Lade sie erneut herunter.",  // UpdatesIntegrityFailed
    "Das Update konnte nicht bestätigt werden. Prüfe erneut oder öffne die Release-Seite über Neuerungen.",  // UpdatesMetadataFailed
    "Download konnte nicht gespeichert werden. Prüfe Speicherplatz und Ordnerberechtigungen.",  // UpdatesStorageFailed
    "Schließe das Spiel und andere Launcher, die diesen Ordner verwenden, und versuche es erneut.",  // UpdatesCloseGame
    "Der Installer konnte nicht gestartet werden. Versuche es erneut oder öffne die Release-Seite über Neuerungen.",  // UpdatesInstallFailed
    "Einstellungen konnten nicht gespeichert werden. Korrigiere vor der Installation die Dateiberechtigungen.",  // UpdatesSaveFailed
    "Die Installationsregistrierung hat sich geändert. Schließe den Launcher, öffne ihn erneut und versuche es noch einmal.",  // UpdatesNotInstalled
    "Herunterladen und installieren",  // UpdatesDownloadInstall
    "Installer starten",  // UpdatesLaunchInstaller
    "Der Installer wird heruntergeladen und überprüft. Danach fragen wir erneut, bevor der Launcher geschlossen und der Installationsassistent zum Aktualisieren des Spiels geöffnet wird.",  // UpdatesConfirmDownload
    "Der Installer wurde heruntergeladen und überprüft. Jetzt starten? Deine Einstellungen werden gespeichert und der Launcher wird geschlossen. Der Assistent führt dich durch das Update. Schließe vorher das Spiel.",  // UpdatesConfirmInstall
    "Diese Kopie ist portabel. Der Installer registriert sie im selben Ordner als Windows-Anwendung. Dieser Vorgang verwendet keine ZIP-Datei.",  // UpdatesPortableConversion
    "Installationsordner",  // UpdatesDestination
    "Mit dem Download fortfahren?",  // UpdatesContinue
    "Ja",  // UpdatesYes
    "Nein",  // UpdatesNo
    "Download bereit. Klicke auf Herunterladen und installieren, um den Installer zu öffnen.",  // UpdatesDownloadReady
    "Für diese Version gibt es keinen Windows-Installer. Siehe Neuerungen.",  // UpdatesNoInstaller
    "Download abbrechen",  // UpdatesCancelDownload
    "Dieser Ordner enthält nicht den vollständigen PC-Port. Verwende den Launcher im Ordner mit narutorise.exe und narutorise_launcher.exe.",  // UpdatesMissingPortFiles
};

}  // namespace localization_strings
