#pragma once

namespace localization_strings {

inline constexpr const char* kFr[] = {
    "Naruto: Rise of a Ninja",  // WindowTitle
    "NARUTO: RISE OF A NINJA",  // AppTitle
    "Port PC recompilé (ReXGlue)",  // AppSubtitle
    "JOUER",  // TabPlay
    "PARAMÈTRES",  // TabSettings
    "FICHIERS",  // TabGameFiles
    "Qualité graphique",  // GroupGraphics
    "Commandes",  // GroupControls
    "Écran :",  // DisplayLabel
    "Format d'image :",  // AspectLabel
    "16:9 - Standard (original)",  // Aspect16x9
    "21:9 - UltraWide (3440x1440)",  // Aspect21x9
    "32:9 - Super UltraWide (5120x1440)",  // Aspect32x9
    "Ajuste le champ de vision horizontal de la caméra 3D sans étirer ni déformer les personnages.",  // AspectTooltip
    "Résolution :",  // ResolutionLabel
    "1280x720 - 1x (originale)",  // Res1x
    "2560x1440 - 2x (recommandée)",  // Res2x
    "3840x2160 - 3x (4K UHD)",  // Res3x
    "5120x2880 - 4x (5K UHD)",  // Res4x
    "Moteur de rendu :",  // RendererLabel
    "Filtrage des textures :",  // TextureFilteringLabel
    "Par défaut",  // AnisoDefault
    "Anticrénelage :",  // AntiAliasingLabel
    "Désactivé",  // AaOff
    "FXAA",  // AaFxaa
    "FXAA extrême",  // AaFxaaExtreme
    "Post-traitement :",  // PostProcessingLabel
    "Bilinéaire (par défaut)",  // PostBilinear
    "CAS (netteté)",  // PostCas
    "FSR EASU (mise à l'échelle)",  // PostFsr
    "Tramage",  // DitherLabel
    "Compteur FPS",  // FpsOverlayLabel
    "Plein écran",  // FullscreenLabel
    "VSync (limite à 60 Hz)",  // VsyncLabel
    "Laissez cette option activée pour conserver la vitesse correcte des animations et de la physique.",  // VsyncTooltip
    "Utiliser SDL pour les manettes (Xbox / PlayStation)",  // SdlInputLabel
    "Manette :",  // ControllerStatusLabel
    "Aucune manette détectée (clavier et souris)",  // NoControllerDetected
    "Passer les vidéos d'introduction (logos Ubisoft)",  // SkipIntroLabel
    "Langue :",  // LanguageLabel
    "JOUER",  // BtnPlay
    "Réglages recommandés",  // BtnResetRecommended
    "Enregistrer",  // BtnSaveSettings
    "Ouvrir le dossier du jeu",  // BtnOpenFolder
    "Les paramètres seront appliqués au lancement du jeu.",  // SettingsAppliedNotice
    "Paramètres enregistrés.",  // SettingsSavedNotice
    "Jeu prêt",  // StatusReady
    "Fichiers du jeu introuvables",  // StatusNotFound
    "Fichiers du jeu trouvés dans : %s",  // FilesFoundIn
    "Placez les fichiers Xbox 360 extraits dans 'game' ou sélectionnez leur dossier dans FICHIERS. L'extraction ISO est disponible dans l'installateur.",  // FilesMissingDesc
    "Les fichiers originaux extraits du jeu Xbox 360 sont nécessaires. Extrayez votre ISO avec l'installateur ou copiez les fichiers dans 'game'.",  // FilesNotice
    "Chemin actuel : %s",  // CurrentPathLabel
    "(Aucun dossier trouvé)",  // NoneDetected
    "Sélectionner le dossier du jeu extrait...",  // BtnSelectFolder
    "Rétablir le chemin par défaut",  // BtnResetDefault
    "Chemin par défaut rétabli.",  // PathResetMsg
    "Les paramètres sont enregistrés au lancement du jeu ou à la fermeture du lanceur.",  // FooterNotice
    "Jeu lancé !",  // LaunchSuccessMsg
    "Impossible de démarrer le processus : ",  // LaunchFailedMsg
    "Exécutable du jeu introuvable dans : ",  // ExeNotFoundMsg
    "Chemin mis à jour : ",  // PathUpdatedMsg
    "Votre aventure commence ici.",  // PlayHeading
    "Retournez au village de Konoha.",  // PlayIntro
    "Fichiers du jeu détectés. Vous pouvez jouer.",  // ReadyDetail
    "Gérer les fichiers du jeu",  // ManageFiles
    "Adaptez le jeu à votre PC.",  // SettingsIntro
    "Associez vos fichiers originaux au port PC.",  // FilesIntro
    "Dossier du jeu",  // FolderHeading
    "Sélectionnez le dossier contenant default.xex. Vos fichiers originaux ne seront ni déplacés ni supprimés.",  // FolderHint
    "Vous avez une ISO ?",  // SetupHeading
    "Relancez l'installateur pour extraire votre ISO Xbox 360 originale. L'importation ISO est disponible dans l'installateur.",  // SetupHint
    "Paramètres recommandés appliqués.",  // RecommendedApplied
    "Impossible d'enregistrer. Vérifiez les droits d'écriture du dossier.",  // SaveFailed
    "L'exécutable PC du jeu est absent. Réinstallez le port avec l'installateur.",  // ExeMissing
    "Port PC introuvable",  // ExeMissingStatus
    "Fichiers du jeu détectés",  // FilesDetected
    "Fermer le lanceur",  // Exit
    "DLC",  // TabDlc
    "Contenu téléchargeable",  // DlcHeading
    "Installez les packs de personnages DLC de Naruto: Rise of a Ninja.",  // DlcIntro
    "DLC installés",  // DlcInstalledHeading
    "Aucun DLC installé.",  // DlcNoInstalled
    "Installer un dossier DLC",  // DlcInstallLoose
    "Sélectionnez un dossier de DLC. Les paquets Xbox 360 (.live/.con/.pirs) seront installés au lancement du jeu ; les dossiers déjà extraits sont copiés directement.",  // DlcInstallLooseHint
    "Sélectionner le dossier DLC...",  // DlcSelectLooseFolder
    "DLC installé.",  // DlcInstallSuccess
    "Échec de l'installation du DLC :",  // DlcInstallFailed
    "Paquets DLC prêts. Ils seront installés au prochain lancement du jeu.",  // DlcStfsStaged
    "Ouvrir le dossier DLC",  // DlcOpenContentFolder
    "Préparer les dossiers DLC",  // DlcPrepareFolders
    "Les dossiers DLC sont prêts.",  // DlcFoldersReady
    "Naruto: Rise of a Ninja - Installation",  // InstWindowTitle
    "Bienvenue dans l'installateur du port PC de Naruto: Rise of a Ninja",  // InstWelcomeTitle
    "Cet assistant installe le port PC et importe vos fichiers originaux du jeu Xbox 360.",  // InstWelcomeIntro
    "1. Choisissez le dossier d'installation",  // InstWelcomeStep1
    "2. Sélectionnez votre ISO de Naruto: Rise of a Ninja",  // InstWelcomeStep2
    "3. Les fichiers du jeu sont extraits automatiquement",  // InstWelcomeStep3
    "4. Des raccourcis sont créés (facultatif)",  // InstWelcomeStep4
    "5. Jouez depuis le lanceur",  // InstWelcomeStep5
    "Vous devez posséder le jeu original. Aucun fichier du jeu n'est distribué avec ce port.",  // InstWelcomeLegal
    "Fichiers d'installation introuvables. Exécutez cet installateur depuis son dossier de distribution ou utilisez l'installateur généré.",  // InstPayloadMissing
    "Dossier d'installation",  // InstDestTitle
    "Les fichiers du port seront copiés dans ce dossier.",  // InstDestHint
    "Parcourir...",  // InstBrowse
    "Suivant",  // InstNext
    "Précédent",  // InstBack
    "INSTALLER",  // InstInstall
    "Terminer",  // InstFinish
    "Image ISO du jeu",  // InstIsoTitle
    "Sélectionnez votre ISO Xbox 360 originale (.iso).",  // InstIsoHint
    "Sélectionner l'ISO de Naruto: Rise of a Ninja",  // InstIsoDialog
    "Vérification de l'ISO...",  // InstIsoChecking
    "ISO Xbox 360 valide : %u fichiers (%.2f Go)",  // InstIsoValid
    "Le fichier sélectionné n'est pas une ISO Xbox 360 valide.",  // InstIsoInvalid
    "default.xex est introuvable dans l'ISO.",  // InstIsoNoXex
    "Passer l'extraction (fichiers du jeu déjà présents dans le dossier de destination)",  // InstIsoSkipCheck
    "Sélectionnez une ISO valide ou cochez 'Passer l'extraction' si les fichiers du jeu sont déjà présents.",  // InstIsoRequired
    "Indiquez un dossier d'installation valide.",  // InstInvalidDest
    "Installation",  // InstProgressTitle
    "Préparation de l'installation...",  // InstPreparing
    "Suppression des anciens fichiers du jeu...",  // InstRemovingOld
    "Extraction de l'ISO (cela peut prendre quelques minutes)...",  // InstExtractingIso
    "Copie des fichiers du port...",  // InstCopyingFiles
    "Écriture de la configuration...",  // InstWritingConfig
    "Enregistrement dans la liste des programmes...",  // InstRegistering
    "Installation terminée !",  // InstComplete
    "Installation annulée.",  // InstCanceled
    "Échec de l'installation",  // InstFailedTitle
    "Installation terminée",  // InstFinishTitle
    "Installé dans : %s",  // InstInstalledTo
    "Fichiers du jeu dans : %s",  // InstGameFilesAt
    "Créer un raccourci sur le bureau",  // InstShortcutDesktop
    "Créer un raccourci dans le menu Démarrer",  // InstShortcutStartMenu
    "Lancer Naruto maintenant",  // InstLaunchNow
    "Naruto: Rise of a Ninja - Désinstallation",  // UnWindowTitle
    "Ceci supprime les fichiers du port, les raccourcis et l'entrée dans la liste des programmes.",  // UnIntro
    "Désinstaller (conserver les fichiers du jeu)",  // UnKeepFiles
    "Tout désinstaller",  // UnRemoveAll
    "Annuler",  // UnCancel
    "Désinstallation terminée. Certains fichiers peuvent rester jusqu'à la fermeture de cette fenêtre.",  // UnDone
    "Échec de la désinstallation : %s",  // UnFailed
    "Jeu",  // GroupGame
    "Langue du jeu :",  // GameLanguageLabel
    "Anglais",  // GameLanguageEnglish
    "Français",  // GameLanguageFrench
    "Allemand",  // GameLanguageGerman
    "Espagnol",  // GameLanguageSpanish
    "Italien",  // GameLanguageItalian
    "Personnalisée (%u)",  // GameLanguageCustom
    "Appliquée au prochain lancement. Les textes et voix disponibles dépendent des fichiers du jeu.",  // GameLanguageHint
    "Langue de l'interface du lanceur",  // LauncherLanguageTooltip
};

}  // namespace localization_strings
