#pragma once

namespace localization_strings {

inline constexpr const char* kDe[] = {
    "Naruto: Rise of a Ninja",  // WindowTitle
    "NARUTO: RISE OF A NINJA",  // AppTitle
    "PC-Port durch Rekompilierung (ReXGlue)",  // AppSubtitle
    "SPIELEN",  // TabPlay
    "OPTIONEN",  // TabSettings
    "SPIELDATEIEN",  // TabGameFiles
    "Grafikqualität",  // GroupGraphics
    "Steuerung",  // GroupControls
    "Bildschirm:",  // DisplayLabel
    "Seitenverhältnis:",  // AspectLabel
    "16:9 - Standard (Original)",  // Aspect16x9
    "21:9 - UltraWide (3440x1440)",  // Aspect21x9
    "32:9 - Super UltraWide (5120x1440)",  // Aspect32x9
    "Passt das horizontale Sichtfeld der 3D-Kamera an, ohne die Figuren zu strecken oder zu verzerren.",  // AspectTooltip
    "Auflösung:",  // ResolutionLabel
    "1280x720 - 1x (Original)",  // Res1x
    "2560x1440 - 2x (empfohlen)",  // Res2x
    "3840x2160 - 3x (4K UHD)",  // Res3x
    "5120x2880 - 4x (5K UHD)",  // Res4x
    "Renderer:",  // RendererLabel
    "Texturfilter:",  // TextureFilteringLabel
    "Standard",  // AnisoDefault
    "Kantenglättung:",  // AntiAliasingLabel
    "Aus",  // AaOff
    "FXAA",  // AaFxaa
    "FXAA Extrem",  // AaFxaaExtreme
    "Nachbearbeitung:",  // PostProcessingLabel
    "Bilinear (Standard)",  // PostBilinear
    "CAS (Schärfen)",  // PostCas
    "FSR EASU (Skalierung)",  // PostFsr
    "Dithering",  // DitherLabel
    "FPS-Anzeige",  // FpsOverlayLabel
    "Vollbild",  // FullscreenLabel
    "VSync (60-Hz-Limit)",  // VsyncLabel
    "Aktiviert lassen, damit Animationen und Physik mit der richtigen Geschwindigkeit laufen.",  // VsyncTooltip
    "SDL für Controller verwenden (Xbox / PlayStation)",  // SdlInputLabel
    "Controller:",  // ControllerStatusLabel
    "Kein Controller erkannt (Tastatur und Maus)",  // NoControllerDetected
    "Intro-Videos überspringen (Ubisoft-Logos)",  // SkipIntroLabel
    "Sprache:",  // LanguageLabel
    "SPIEL STARTEN",  // BtnPlay
    "Empfohlene Optionen",  // BtnResetRecommended
    "Speichern",  // BtnSaveSettings
    "Spielordner öffnen",  // BtnOpenFolder
    "Die Optionen werden beim Spielstart angewendet.",  // SettingsAppliedNotice
    "Optionen gespeichert.",  // SettingsSavedNotice
    "Spiel bereit",  // StatusReady
    "Spieldateien nicht gefunden",  // StatusNotFound
    "Spieldateien gefunden in: %s",  // FilesFoundIn
    "Lege die extrahierten Xbox-360-Dateien in 'game' ab oder wähle ihren Ordner unter SPIELDATEIEN. ISO-Dateien können im Installationsprogramm extrahiert werden.",  // FilesMissingDesc
    "Die extrahierten Originaldateien des Xbox-360-Spiels werden benötigt. Extrahiere die ISO mit dem Installationsprogramm oder kopiere die Dateien in 'game'.",  // FilesNotice
    "Aktueller Pfad: %s",  // CurrentPathLabel
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
    "Passe das Spiel an deinen PC an.",  // SettingsIntro
    "Verbinde deine Originaldateien mit dem PC-Port.",  // FilesIntro
    "Spielordner",  // FolderHeading
    "Wähle den Ordner mit default.xex. Deine Originaldateien werden weder verschoben noch gelöscht.",  // FolderHint
    "Du hast eine ISO-Datei?",  // SetupHeading
    "Starte das Installationsprogramm erneut, um deine originale Xbox-360-ISO zu extrahieren. ISO-Import ist im Installationsprogramm verfügbar.",  // SetupHint
    "Empfohlene Optionen angewendet.",  // RecommendedApplied
    "Speichern fehlgeschlagen. Prüfe die Schreibrechte des Ordners.",  // SaveFailed
    "Das PC-Spielprogramm fehlt. Installiere den Port erneut.",  // ExeMissing
    "PC-Port nicht gefunden",  // ExeMissingStatus
    "Spieldateien erkannt",  // FilesDetected
    "Launcher schließen",  // Exit
    "DLC",  // TabDlc
    "Herunterladbare Inhalte",  // DlcHeading
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
    "Spielsprache:",  // GameLanguageLabel
    "Englisch",  // GameLanguageEnglish
    "Französisch",  // GameLanguageFrench
    "Deutsch",  // GameLanguageGerman
    "Spanisch",  // GameLanguageSpanish
    "Italienisch",  // GameLanguageItalian
    "Benutzerdefiniert (%u)",  // GameLanguageCustom
    "Wird beim nächsten Spielstart angewendet. Verfügbare Texte und Stimmen hängen von deinen Spieldateien ab.",  // GameLanguageHint
    "Sprache der Launcher-Oberfläche",  // LauncherLanguageTooltip
};

}  // namespace localization_strings
