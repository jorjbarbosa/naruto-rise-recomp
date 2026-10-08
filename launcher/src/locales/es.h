#pragma once

namespace localization_strings {

inline constexpr const char* kEs[] = {
    "Naruto: Rise of a Ninja",  // WindowTitle
    "NARUTO: RISE OF A NINJA",  // AppTitle
    "Versión recompilada para PC (ReXGlue)",  // AppSubtitle
    "JUGAR",  // TabPlay
    "AJUSTES",  // TabSettings
    "ARCHIVOS",  // TabGameFiles
    "Calidad gráfica",  // GroupGraphics
    "Controles",  // GroupControls
    "Pantalla:",  // DisplayLabel
    "Relación de aspecto:",  // AspectLabel
    "16:9 - Estándar (original)",  // Aspect16x9
    "21:9 - UltraWide (3440x1440)",  // Aspect21x9
    "32:9 - Super UltraWide (5120x1440)",  // Aspect32x9
    "Ajusta el campo de visión horizontal de la cámara 3D sin estirar ni distorsionar los personajes.",  // AspectTooltip
    "Resolución:",  // ResolutionLabel
    "1280x720 - 1x (original)",  // Res1x
    "2560x1440 - 2x (recomendado)",  // Res2x
    "3840x2160 - 3x (4K UHD)",  // Res3x
    "5120x2880 - 4x (5K UHD)",  // Res4x
    "Renderizador:",  // RendererLabel
    "Filtro de texturas:",  // TextureFilteringLabel
    "Predeterminado",  // AnisoDefault
    "Antialiasing:",  // AntiAliasingLabel
    "Desactivado",  // AaOff
    "FXAA",  // AaFxaa
    "FXAA extremo",  // AaFxaaExtreme
    "Posprocesado:",  // PostProcessingLabel
    "Bilineal (predeterminado)",  // PostBilinear
    "CAS (nitidez)",  // PostCas
    "FSR EASU (escalado)",  // PostFsr
    "Tramado",  // DitherLabel
    "Indicador de FPS",  // FpsOverlayLabel
    "Pantalla completa",  // FullscreenLabel
    "VSync (límite de 60 Hz)",  // VsyncLabel
    "Mantén esta opción activada para conservar la velocidad correcta de las animaciones y la física.",  // VsyncTooltip
    "Usar SDL para mandos (Xbox / PlayStation)",  // SdlInputLabel
    "Mando:",  // ControllerStatusLabel
    "No se detectó ningún mando (teclado y ratón)",  // NoControllerDetected
    "Omitir vídeos de introducción (logos de Ubisoft)",  // SkipIntroLabel
    "Idioma:",  // LanguageLabel
    "JUGAR",  // BtnPlay
    "Ajustes recomendados",  // BtnResetRecommended
    "Guardar ajustes",  // BtnSaveSettings
    "Abrir carpeta del juego",  // BtnOpenFolder
    "Los ajustes se aplicarán al iniciar el juego.",  // SettingsAppliedNotice
    "Ajustes guardados.",  // SettingsSavedNotice
    "Juego listo",  // StatusReady
    "No se encontraron los archivos del juego",  // StatusNotFound
    "Archivos del juego encontrados en: %s",  // FilesFoundIn
    "Coloca los archivos extraídos de Xbox 360 en 'game' o selecciona su carpeta en ARCHIVOS. La extracción de ISO está disponible en el instalador.",  // FilesMissingDesc
    "Se necesitan los archivos originales extraídos del juego de Xbox 360. Extrae tu ISO con el instalador o copia los archivos en 'game'.",  // FilesNotice
    "Ruta actual: %s",  // CurrentPathLabel
    "(No se encontró ninguna carpeta)",  // NoneDetected
    "Seleccionar carpeta del juego extraído...",  // BtnSelectFolder
    "Restablecer ruta predeterminada",  // BtnResetDefault
    "Ruta predeterminada restablecida.",  // PathResetMsg
    "Los ajustes se guardan al jugar o cerrar el launcher.",  // FooterNotice
    "¡Juego iniciado!",  // LaunchSuccessMsg
    "No se pudo iniciar el proceso: ",  // LaunchFailedMsg
    "No se encontró el ejecutable del juego en: ",  // ExeNotFoundMsg
    "Ruta actualizada: ",  // PathUpdatedMsg
    "Tu aventura comienza aquí.",  // PlayHeading
    "Vuelve a la Aldea Oculta de la Hoja.",  // PlayIntro
    "Archivos del juego detectados. Ya puedes jugar.",  // ReadyDetail
    "Gestionar archivos del juego",  // ManageFiles
    "Adapta el juego a tu PC.",  // SettingsIntro
    "Conecta tus archivos originales con la versión para PC.",  // FilesIntro
    "Carpeta del juego",  // FolderHeading
    "Selecciona la carpeta que contiene default.xex. Tus archivos originales no se mueven ni se eliminan.",  // FolderHint
    "¿Tienes una ISO?",  // SetupHeading
    "Ejecuta de nuevo el instalador para extraer tu ISO original de Xbox 360. La importación de ISO está disponible en el instalador.",  // SetupHint
    "Ajustes recomendados aplicados.",  // RecommendedApplied
    "No se pudo guardar. Comprueba los permisos de escritura de la carpeta.",  // SaveFailed
    "Falta el ejecutable del juego para PC. Vuelve a instalar la versión para PC.",  // ExeMissing
    "No se encontró la versión para PC",  // ExeMissingStatus
    "Archivos del juego detectados",  // FilesDetected
    "Cerrar launcher",  // Exit
    "DLC",  // TabDlc
    "Contenido descargable",  // DlcHeading
    "Instala los paquetes de personajes DLC de Naruto: Rise of a Ninja.",  // DlcIntro
    "DLC instalados",  // DlcInstalledHeading
    "Todavía no hay DLC instalados.",  // DlcNoInstalled
    "Instalar carpeta de DLC",  // DlcInstallLoose
    "Selecciona una carpeta de DLC. Los paquetes de Xbox 360 (.live/.con/.pirs) se instalan al iniciar el juego; las carpetas ya extraídas se copian directamente.",  // DlcInstallLooseHint
    "Seleccionar carpeta de DLC...",  // DlcSelectLooseFolder
    "DLC instalado.",  // DlcInstallSuccess
    "Error al instalar el DLC:",  // DlcInstallFailed
    "Paquetes de DLC preparados. Se instalarán al iniciar el juego.",  // DlcStfsStaged
    "Abrir carpeta de DLC",  // DlcOpenContentFolder
    "Preparar carpetas de DLC",  // DlcPrepareFolders
    "Las carpetas de DLC están listas.",  // DlcFoldersReady
    "Naruto: Rise of a Ninja - Instalación",  // InstWindowTitle
    "Bienvenido al instalador de Naruto: Rise of a Ninja para PC",  // InstWelcomeTitle
    "Este asistente instala la versión para PC e importa tus archivos originales del juego de Xbox 360.",  // InstWelcomeIntro
    "1. Elige la carpeta de instalación",  // InstWelcomeStep1
    "2. Selecciona tu ISO de Naruto: Rise of a Ninja",  // InstWelcomeStep2
    "3. Los archivos del juego se extraen automáticamente",  // InstWelcomeStep3
    "4. Se crean accesos directos (opcional)",  // InstWelcomeStep4
    "5. Juega desde el launcher",  // InstWelcomeStep5
    "Debes tener el juego original. Esta versión no incluye archivos del juego.",  // InstWelcomeLegal
    "No se encontraron los archivos de instalación. Ejecuta este instalador desde su carpeta de distribución o usa el instalador generado.",  // InstPayloadMissing
    "Carpeta de instalación",  // InstDestTitle
    "Los archivos de la versión para PC se copiarán en esta carpeta.",  // InstDestHint
    "Examinar...",  // InstBrowse
    "Siguiente",  // InstNext
    "Atrás",  // InstBack
    "INSTALAR",  // InstInstall
    "Finalizar",  // InstFinish
    "Imagen ISO del juego",  // InstIsoTitle
    "Selecciona tu ISO original de Xbox 360 (.iso).",  // InstIsoHint
    "Seleccionar ISO de Naruto: Rise of a Ninja",  // InstIsoDialog
    "Comprobando la ISO...",  // InstIsoChecking
    "ISO válida de Xbox 360: %u archivos (%.2f GB)",  // InstIsoValid
    "El archivo seleccionado no es una ISO válida de Xbox 360.",  // InstIsoInvalid
    "No se encontró default.xex en la ISO.",  // InstIsoNoXex
    "Omitir extracción (los archivos del juego ya están en la carpeta de destino)",  // InstIsoSkipCheck
    "Selecciona una ISO válida o marca 'Omitir extracción' si los archivos del juego ya están disponibles.",  // InstIsoRequired
    "Introduce una carpeta de instalación válida.",  // InstInvalidDest
    "Instalando",  // InstProgressTitle
    "Preparando la instalación...",  // InstPreparing
    "Eliminando archivos anteriores del juego...",  // InstRemovingOld
    "Extrayendo la ISO (puede tardar unos minutos)...",  // InstExtractingIso
    "Copiando los archivos de la versión para PC...",  // InstCopyingFiles
    "Guardando la configuración...",  // InstWritingConfig
    "Registrando en la lista de programas...",  // InstRegistering
    "¡Instalación completada!",  // InstComplete
    "Instalación cancelada.",  // InstCanceled
    "Error de instalación",  // InstFailedTitle
    "Instalación completada",  // InstFinishTitle
    "Instalado en: %s",  // InstInstalledTo
    "Archivos del juego en: %s",  // InstGameFilesAt
    "Crear acceso directo en el escritorio",  // InstShortcutDesktop
    "Crear acceso directo en el menú Inicio",  // InstShortcutStartMenu
    "Ejecutar el launcher de Naruto ahora",  // InstLaunchNow
    "Naruto: Rise of a Ninja - Desinstalación",  // UnWindowTitle
    "Se eliminarán los archivos de la versión para PC, los accesos directos y la entrada de la lista de programas.",  // UnIntro
    "Desinstalar (conservar archivos del juego)",  // UnKeepFiles
    "Desinstalar todo",  // UnRemoveAll
    "Cancelar",  // UnCancel
    "Desinstalación completada. Algunos archivos pueden permanecer hasta que se cierre esta ventana.",  // UnDone
    "Error al desinstalar: %s",  // UnFailed
    "Juego",  // GroupGame
    "Idioma del juego:",  // GameLanguageLabel
    "Inglés",  // GameLanguageEnglish
    "Francés",  // GameLanguageFrench
    "Alemán",  // GameLanguageGerman
    "Español",  // GameLanguageSpanish
    "Italiano",  // GameLanguageItalian
    "Personalizado (%u)",  // GameLanguageCustom
    "Se aplica al iniciar el juego. Los textos y voces disponibles dependen de los archivos del juego.",  // GameLanguageHint
    "Idioma de la interfaz del launcher",  // LauncherLanguageTooltip
};

}  // namespace localization_strings
