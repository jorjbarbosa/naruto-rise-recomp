#pragma once

namespace localization_strings {

inline constexpr const char* kFr[] = {
    "Naruto: Rise of a Ninja",  // WindowTitle
    "JOUER",  // TabPlay
    "PARAMÈTRES",  // TabSettings
    "FICHIERS",  // TabGameFiles
    "Qualité",  // GroupQuality
    "Commandes",  // GroupControls
    "Format d'image",  // AspectLabel
    "16:9 - Standard (original)",  // Aspect16x9
    "21:9 - UltraWide",  // Aspect21x9
    "32:9 - Super UltraWide",  // Aspect32x9
    "Ajuste le champ de vision horizontal de la caméra 3D sans étirer ni déformer les personnages.",  // AspectDescription
    "Résolution",  // ResolutionLabel
    "Moteur de rendu",  // RendererLabel
    "Filtrage des textures",  // TextureFilteringLabel
    "Par défaut",  // AnisoDefault
    "FXAA",  // FxaaLabel
    "Désactivé",  // FxaaOff
    "Normal",  // FxaaNormal
    "Extrême",  // FxaaExtreme
    "Post-traitement",  // PostProcessingLabel
    "Bilinéaire (par défaut)",  // PostBilinear
    "CAS (netteté)",  // PostCas
    "FSR EASU (mise à l'échelle)",  // PostFsr
    "Tramage",  // DitherLabel
    "Compteur FPS",  // FpsOverlayLabel
    "Plein écran",  // FullscreenLabel
    "VSync",  // VsyncLabel
    "Laissez cette option activée pour conserver la vitesse correcte des animations et de la physique.",  // VsyncDescription
    "Utiliser SDL pour les manettes (Xbox / PlayStation)",  // SdlInputLabel
    "Manette",  // ControllerLabel
    "Aucune manette détectée (clavier et souris)",  // NoControllerDetected
    "Passer les vidéos d'introduction (logos Ubisoft)",  // SkipIntroLabel
    "JOUER",  // BtnPlay
    "Réglages recommandés",  // BtnResetRecommended
    "Ouvrir le dossier du jeu",  // BtnOpenFolder
    "Les paramètres seront appliqués au lancement du jeu.",  // SettingsAppliedNotice
    "Jeu prêt",  // StatusReady
    "Fichiers du jeu introuvables",  // StatusNotFound
    "Placez les fichiers Xbox 360 extraits dans 'game' ou sélectionnez leur dossier dans FICHIERS. L'extraction ISO est disponible dans l'installateur.",  // FilesMissingDesc
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
    "Associez vos fichiers originaux au port PC.",  // FilesIntro
    "Dossier du jeu",  // FolderHeading
    "Sélectionnez le dossier contenant default.xex. Vos fichiers originaux ne seront ni déplacés ni supprimés.",  // FolderHint
    "Vous avez une ISO ?",  // SetupHeading
    "Relancez l'installateur pour extraire votre ISO Xbox 360 originale. L'importation ISO est disponible dans l'installateur.",  // SetupHint
    "Paramètres recommandés appliqués.",  // RecommendedApplied
    "L'exécutable PC du jeu est absent. Réinstallez le port avec l'installateur.",  // ExeMissing
    "Port PC introuvable",  // ExeMissingStatus
    "Fichiers du jeu détectés",  // FilesDetected
    "DLC",  // TabDlc
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
    "Langue du jeu",  // GameLanguageLabel
    "Anglais",  // GameLanguageEnglish
    "Français",  // GameLanguageFrench
    "Allemand",  // GameLanguageGerman
    "Espagnol",  // GameLanguageSpanish
    "Italien",  // GameLanguageItalian
    "Personnalisée (%u)",  // GameLanguageCustom
    "Appliquée au prochain lancement. Les traductions installées dans mods apparaissent ici et remplacent les textes uniquement si elles sont sélectionnées. Les voix disponibles dépendent des fichiers du jeu.",  // GameLanguageDescription
    "Langue de l'interface du lanceur",  // LauncherLanguageTooltip
    "Xenos (GPU émulé)",  // RendererXenos
    "Natif",  // RendererNative
    "API graphique utilisée par le jeu : Direct3D 12 ou Vulkan. Appliquée au prochain lancement.",  // RendererDescription
    "Non disponible avec le moteur de rendu natif. Choisissez Xenos dans Moteur de rendu pour l'utiliser.",  // XenosOnlyReason
    "MSAA",  // MsaaLabel
    "4x (original)",  // MsaaOriginal
    "Multi-échantillonnage des scènes 3D du jeu. Le 8x demande un GPU plus puissant.",  // MsaaDescription
    "AA de transparence",  // TransparencyAaLabel
    "Adoucit les bords du feuillage, des clôtures et des autres textures découpées (alpha to coverage).",  // TransparencyAaDescription
    "Fréquence d'images",  // FrameRateLabel
    "30 est la fréquence d'origine du jeu. Au-delà, la logique garde son rythme et les images intermédiaires sont interpolées. Avec la VSync, elle s'arrête à celle de l'écran ; les fréquences élevées demandent un CPU plus rapide.",  // FrameRateDescription
    "Luminosité",  // BrightnessLabel
    "50 correspond à la luminosité d'origine du jeu.",  // BrightnessDescription
    "Latence d'image",  // FrameLatencyLabel
    "1 (minimale)",  // FrameLatencyLowest
    "Images en file d'attente avant l'affichage. 1 offre la latence la plus faible ; 2 ou 3 peuvent lisser des temps d'image irréguliers.",  // FrameLatencyDescription
    "Triple tampon",  // TripleBufferingLabel
    "Trois tampons d'affichage au lieu de deux.",  // TripleBufferingDescription
    "Synchronise l'affichage avec l'écran. Avec le moteur de rendu natif, la vitesse du jeu ne dépend pas de la VSync.",  // VsyncNativeDescription
    "Carte graphique",  // GraphicsDeviceLabel
    "Automatique",  // GraphicsDeviceAuto
    "La carte graphique utilisée par le jeu. Automatique laisse le choix au moteur de rendu. Sur les portables à deux cartes graphiques, choisissez la carte dédiée (NVIDIA ou AMD).",  // GraphicsDeviceDescription
    "Affichage",  // GroupDisplay
    "Avancé",  // GroupAdvanced
    "Xenos uniquement",  // XenosOnlyTag
    "natif uniquement",  // NativeOnlyTag
    "Uniquement avec le moteur de rendu natif. Choisissez Natif dans Moteur de rendu pour l'utiliser.",  // NativeOnlyReason
    "La résolution à laquelle le jeu est dessiné, en multiples des 1280×720 d'origine. Des valeurs plus élevées donnent une image plus nette et demandent une carte graphique plus puissante.",  // ResolutionDescription
    "Le filtrage anisotrope garde les textures nettes sur les surfaces vues de biais, comme les sols et les routes.",  // TextureFilteringDescription
    "Adoucit les bords crénelés en filtrant l'image finale. Extrême lisse davantage et peut flouter les détails fins.",  // FxaaDescription
    "La façon dont l'image est adaptée à l'écran. CAS accentue la netteté, FSR agrandit l'image en l'affinant et le filtrage bilinéaire la garde douce.",  // PostProcessingDescription
    "Ajoute un bruit fin qui masque les bandes de couleur dans les dégradés.",  // DitherDescription
    "Affiche la fréquence et le temps d'image dans un coin de l'écran. F1 l'active ou le désactive en jeu.",  // FpsOverlayDescription
    "Lance le jeu en plein écran. Désactivé, il s'ouvre dans une fenêtre.",  // FullscreenDescription
    "Passe les logos affichés au démarrage du jeu. La cinématique d'ouverture est conservée.",  // SkipIntroDescription
    "Lit les manettes via SDL, qui reconnaît les manettes Xbox, PlayStation et d'autres. Désactivé, le jeu utilise XInput, qui ne reconnaît que les manettes compatibles Xbox.",  // SdlInputDescription
    "La manette connectée en ce moment, avec le nom que lui donne SDL.",  // ControllerDescription
    "Utiliser le clavier et la souris",  // KeyboardMouseLabel
    "Le clavier et la souris remplacent une manette Xbox et fonctionnent avec une manette connectée. Avec la souris sur la caméra, le curseur reste dans la fenêtre du jeu tant qu'elle est active ; Alt+Tab le libère. Le jeu affiche toujours les boutons de la manette.",  // KeyboardMouseDescription
    "La souris déplace la caméra",  // MouseCameraLabel
    "Déplace la caméra (le stick droit) avec la souris. Désactivé, la caméra ne bouge qu'avec les touches du stick droit.",  // MouseCameraDescription
    "Sensibilité",  // MouseSensitivityLabel
    "La vitesse à laquelle la caméra suit la souris. 100 % est la valeur par défaut.",  // MouseSensitivityDescription
    "Afficher les touches dans les textes",  // KeyPromptsLabel
    "Affiche les touches attribuées au lieu des boutons de la manette dans les astuces et tutoriels du jeu, par exemple [Space] pour A. Fonctionne avec le texte anglais du jeu et les mods de traduction.",  // KeyPromptsDescription
    "Touches",  // KeysLabel
    "Les touches et boutons de la souris de chaque commande de la manette, deux par commande.",  // KeysDescription
    "Configurer les touches…",  // MapKeysButton
    "Configurer les touches",  // MapKeysTitle
    "Commandes › Clavier et souris",  // MapKeysPath
    "Boutons",  // KeysButtonsHeading
    "Stick gauche",  // LeftStickName
    "Stick droit",  // RightStickName
    "Croix directionnelle",  // DpadName
    "+ souris",  // KeysPlusMouse
    "Haut",  // DirectionUp
    "Bas",  // DirectionDown
    "Gauche",  // DirectionLeft
    "Droite",  // DirectionRight
    "Cliquez puis appuyez sur la nouvelle touche (5 s). Clic droit pour effacer. Une touche déjà utilisée quitte sa commande précédente.",  // KeysHint
    "Appuyez… %d",  // KeyCapturePrompt
    "Rétablir les touches par défaut",  // KeysRestoreDefaults
    "Terminé",  // KeysDone
    "%s a été retirée de %s.",  // KeyMovedStatus
    "%s est utilisée par le jeu et ne peut pas être attribuée.",  // KeyReservedStatus
    "Touches par défaut rétablies.",  // KeysRestoredStatus
    "Espace",  // KeySpaceName
    "Clic gauche",  // MouseLeftName
    "Clic droit",  // MouseRightName
    "Clic milieu",  // MouseMiddleName
    "API graphique native",  // NativeBackendLabel
    "Choisissez Direct3D 12 ou Vulkan. Redémarrage du jeu requis. Vulkan limite la file à une présentation.",  // NativeBackendDescription
    "Vérifier les mises à jour",  // UpdatesCheck
    "Recherche de mises à jour…",  // UpdatesChecking
    "Votre version est à jour.",  // UpdatesCurrent
    "Vérification impossible. Réessayez plus tard.",  // UpdatesFailed
    "Impossible de comparer la version installée.",  // UpdatesUnknownVersion
    "Nouvelle version disponible : %s",  // UpdatesAvailable
    "Nouveautés",  // UpdatesNotes
    "Télécharger",  // UpdatesDownload
    "Installateur Windows",  // UpdatesInstaller
    "ZIP portable",  // UpdatesPortable
    "Plus tard",  // UpdatesLater
    "Ignorer cette version",  // UpdatesIgnore
    "Impossible d’ouvrir le navigateur. Réessayez.",  // UpdatesBrowserFailed
    "Ouvre le téléchargement dans votre navigateur. Fermez le jeu et le lanceur avant d’appliquer la mise à jour.",  // UpdatesDownloadHint
    "Patientez. Les vérifications sont limitées à une par minute.",  // UpdatesCheckCooldown
    "Téléchargement manuel",  // UpdatesManualDownload
    "Téléchargement de la mise à jour…",  // UpdatesDownloading
    "Vérification du téléchargement…",  // UpdatesVerifying
    "Annulation…",  // UpdatesCancelling
    "Annuler",  // UpdatesCancel
    "Téléchargement annulé.",  // UpdatesCancelled
    "Téléchargement vérifié. Le lanceur se fermera et l’installateur mettra à jour ce dossier :",  // UpdatesReadyToInstall
    "Installer et fermer le lanceur",  // UpdatesInstallClose
    "Échec du téléchargement. Vérifiez la connexion et réessayez.",  // UpdatesDownloadFailed
    "Le fichier a échoué à la vérification. Téléchargez-le à nouveau.",  // UpdatesIntegrityFailed
    "Impossible de confirmer cette mise à jour. Réessayez ou ouvrez sa page depuis Nouveautés.",  // UpdatesMetadataFailed
    "Impossible d’enregistrer le téléchargement. Vérifiez l’espace libre et les autorisations du dossier.",  // UpdatesStorageFailed
    "Fermez le jeu et les autres lanceurs utilisant ce dossier, puis réessayez.",  // UpdatesCloseGame
    "Impossible de lancer l’installateur. Réessayez ou ouvrez sa page depuis Nouveautés.",  // UpdatesInstallFailed
    "Impossible d’enregistrer les paramètres. Corrigez les autorisations du fichier avant l’installation.",  // UpdatesSaveFailed
    "L’enregistrement de l’installation a changé. Fermez puis rouvrez le lanceur et réessayez.",  // UpdatesNotInstalled
    "Télécharger et installer",  // UpdatesDownloadInstall
    "Lancer l’installateur",  // UpdatesLaunchInstaller
    "L’installateur sera téléchargé et vérifié. Une seconde confirmation sera demandée avant de fermer le lanceur et d’ouvrir l’assistant pour mettre à jour le jeu.",  // UpdatesConfirmDownload
    "L’installateur a été téléchargé et vérifié. Voulez-vous le lancer maintenant ? Vos paramètres seront enregistrés et le lanceur se fermera. L’assistant vous guidera pendant la mise à jour. Fermez le jeu avant de continuer.",  // UpdatesConfirmInstall
    "Cette copie est portable. L’installateur l’enregistrera comme application Windows dans ce même dossier. Ce processus n’utilise pas le ZIP.",  // UpdatesPortableConversion
    "Dossier d’installation",  // UpdatesDestination
    "Continuer le téléchargement ?",  // UpdatesContinue
    "Oui",  // UpdatesYes
    "Non",  // UpdatesNo
    "Téléchargement prêt. Cliquez sur Télécharger et installer pour lancer l’installateur.",  // UpdatesDownloadReady
    "Aucun installateur Windows pour cette version. Consultez les nouveautés.",  // UpdatesNoInstaller
    "Annuler le téléchargement",  // UpdatesCancelDownload
    "Ce dossier ne contient pas le portage PC complet. Utilisez le lanceur dans le dossier contenant narutorise.exe et narutorise_launcher.exe.",  // UpdatesMissingPortFiles
};

}  // namespace localization_strings
