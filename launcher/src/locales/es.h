#pragma once

namespace localization_strings {

inline constexpr const char* kEs[] = {
    "Naruto: Rise of a Ninja",  // WindowTitle
    "JUGAR",  // TabPlay
    "AJUSTES",  // TabSettings
    "ARCHIVOS",  // TabGameFiles
    "Calidad",  // GroupQuality
    "Controles",  // GroupControls
    "Relación de aspecto",  // AspectLabel
    "16:9 - Estándar (original)",  // Aspect16x9
    "21:9 - UltraWide",  // Aspect21x9
    "32:9 - Super UltraWide",  // Aspect32x9
    "Ajusta el campo de visión horizontal de la cámara 3D sin estirar ni distorsionar los personajes.",  // AspectDescription
    "Resolución",  // ResolutionLabel
    "Renderizador",  // RendererLabel
    "Filtro de texturas",  // TextureFilteringLabel
    "Predeterminado",  // AnisoDefault
    "FXAA",  // FxaaLabel
    "Desactivado",  // FxaaOff
    "Normal",  // FxaaNormal
    "Extremo",  // FxaaExtreme
    "Posprocesado",  // PostProcessingLabel
    "Bilineal (predeterminado)",  // PostBilinear
    "CAS (nitidez)",  // PostCas
    "FSR EASU (escalado)",  // PostFsr
    "Tramado",  // DitherLabel
    "Indicador de FPS",  // FpsOverlayLabel
    "Pantalla completa",  // FullscreenLabel
    "VSync",  // VsyncLabel
    "Mantén esta opción activada para conservar la velocidad correcta de las animaciones y la física.",  // VsyncDescription
    "Usar SDL para mandos (Xbox / PlayStation)",  // SdlInputLabel
    "Mando",  // ControllerLabel
    "No se detectó ningún mando (teclado y ratón)",  // NoControllerDetected
    "Omitir vídeos de introducción (logos de Ubisoft)",  // SkipIntroLabel
    "JUGAR",  // BtnPlay
    "Ajustes recomendados",  // BtnResetRecommended
    "Abrir carpeta del juego",  // BtnOpenFolder
    "Los ajustes se aplicarán al iniciar el juego.",  // SettingsAppliedNotice
    "Juego listo",  // StatusReady
    "No se encontraron los archivos del juego",  // StatusNotFound
    "Coloca los archivos extraídos de Xbox 360 en 'game' o selecciona su carpeta en ARCHIVOS. La extracción de ISO está disponible en el instalador.",  // FilesMissingDesc
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
    "Conecta tus archivos originales con la versión para PC.",  // FilesIntro
    "Carpeta del juego",  // FolderHeading
    "Selecciona la carpeta que contiene default.xex. Tus archivos originales no se mueven ni se eliminan.",  // FolderHint
    "¿Tienes una ISO?",  // SetupHeading
    "Ejecuta de nuevo el instalador para extraer tu ISO original de Xbox 360. La importación de ISO está disponible en el instalador.",  // SetupHint
    "Ajustes recomendados aplicados.",  // RecommendedApplied
    "Falta el ejecutable del juego para PC. Vuelve a instalar la versión para PC.",  // ExeMissing
    "No se encontró la versión para PC",  // ExeMissingStatus
    "Archivos del juego detectados",  // FilesDetected
    "DLC",  // TabDlc
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
    "Idioma del juego",  // GameLanguageLabel
    "Inglés",  // GameLanguageEnglish
    "Francés",  // GameLanguageFrench
    "Alemán",  // GameLanguageGerman
    "Español",  // GameLanguageSpanish
    "Italiano",  // GameLanguageItalian
    "Personalizado (%u)",  // GameLanguageCustom
    "Se aplica al iniciar el juego. Las traducciones instaladas en mods aparecen aquí y solo sustituyen los textos al seleccionarlas. Las voces disponibles dependen de los archivos del juego.",  // GameLanguageDescription
    "Idioma de la interfaz del launcher",  // LauncherLanguageTooltip
    "Xenos (GPU emulada)",  // RendererXenos
    "Nativo",  // RendererNative
    "API gráfica del juego: Direct3D 12 o Vulkan. Se aplica al iniciar el juego.",  // RendererDescription
    "No disponible con el renderizador nativo. Elige Xenos en Renderizador para usarlo.",  // XenosOnlyReason
    "MSAA",  // MsaaLabel
    "4x (original)",  // MsaaOriginal
    "Multimuestreo de las escenas 3D del juego. 8x requiere una GPU más potente.",  // MsaaDescription
    "AA de transparencias",  // TransparencyAaLabel
    "Suaviza los bordes del follaje, las vallas y otras texturas recortadas (alpha to coverage).",  // TransparencyAaDescription
    "Tasa de fotogramas",  // FrameRateLabel
    "30 es la tasa original del juego. Por encima, la lógica mantiene su ritmo y los fotogramas intermedios se interpolan. Con VSync activado, se detiene en la del monitor; las tasas altas requieren una CPU más rápida.",  // FrameRateDescription
    "Brillo",  // BrightnessLabel
    "50 es el brillo original del juego.",  // BrightnessDescription
    "Latencia de fotogramas",  // FrameLatencyLabel
    "1 (mínima)",  // FrameLatencyLowest
    "Fotogramas en cola antes de mostrarse. 1 tiene el menor retraso de entrada; 2 o 3 pueden suavizar tiempos de fotograma irregulares.",  // FrameLatencyDescription
    "Triple búfer",  // TripleBufferingLabel
    "Tres búferes de pantalla en lugar de dos.",  // TripleBufferingDescription
    "Sincroniza la presentación con la pantalla. Con el renderizador nativo, la velocidad del juego no depende de la VSync.",  // VsyncNativeDescription
    "Tarjeta gráfica",  // GraphicsDeviceLabel
    "Automática",  // GraphicsDeviceAuto
    "La tarjeta gráfica en la que se ejecuta el juego. Automática deja la elección al renderizador. En portátiles con dos tarjetas gráficas, elige la dedicada (NVIDIA o AMD).",  // GraphicsDeviceDescription
    "Pantalla",  // GroupDisplay
    "Avanzado",  // GroupAdvanced
    "solo Xenos",  // XenosOnlyTag
    "solo nativo",  // NativeOnlyTag
    "Solo con el renderizador nativo. Elige Nativo en Renderizador para usarlo.",  // NativeOnlyReason
    "La resolución a la que se dibuja el juego, en múltiplos de los 1280×720 originales. Los valores más altos se ven más nítidos y requieren una tarjeta gráfica más potente.",  // ResolutionDescription
    "El filtrado anisotrópico mantiene nítidas las texturas de las superficies vistas en ángulo, como suelos y caminos.",  // TextureFilteringDescription
    "Suaviza los bordes dentados filtrando la imagen final. Extremo suaviza más y puede desenfocar los detalles finos.",  // FxaaDescription
    "Cómo se ajusta la imagen a la pantalla. CAS aumenta la nitidez, FSR la escala con nitidez y Bilineal la deja suave.",  // PostProcessingDescription
    "Añade un ruido fino que oculta las bandas de color en los degradados.",  // DitherDescription
    "Muestra la tasa y el tiempo de fotograma en una esquina de la pantalla. F1 lo activa o desactiva durante el juego.",  // FpsOverlayDescription
    "Ejecuta el juego en pantalla completa. Desactivado, se ejecuta en una ventana.",  // FullscreenDescription
    "Omite los logotipos que aparecen al iniciar el juego. El vídeo de apertura se mantiene.",  // SkipIntroDescription
    "Lee los mandos mediante SDL, que reconoce mandos de Xbox, PlayStation y otros. Desactivado, el juego usa XInput, que solo reconoce mandos compatibles con Xbox.",  // SdlInputDescription
    "El mando conectado ahora, con el nombre que le da SDL.",  // ControllerDescription
    "Usar teclado y ratón",  // KeyboardMouseLabel
    "El teclado y el ratón sustituyen a un mando de Xbox y funcionan junto con un mando conectado. Con el ratón en la cámara, el cursor queda dentro de la ventana del juego mientras tenga el foco; Alt+Tab lo libera. El juego sigue mostrando los botones del mando.",  // KeyboardMouseDescription
    "El ratón mueve la cámara",  // MouseCameraLabel
    "Mueve la cámara (el stick derecho) con el ratón. Desactivado, la cámara solo se mueve con las teclas del stick derecho.",  // MouseCameraDescription
    "Sensibilidad",  // MouseSensitivityLabel
    "La rapidez con que la cámara sigue al ratón. 100 % es el valor predeterminado.",  // MouseSensitivityDescription
    "Teclas",  // KeysLabel
    "Las teclas y los botones del ratón de cada entrada del mando, dos por entrada.",  // KeysDescription
    "Asignar teclas…",  // MapKeysButton
    "Asignar teclas",  // MapKeysTitle
    "Controles › Teclado y ratón",  // MapKeysPath
    "Botones",  // KeysButtonsHeading
    "Stick izquierdo",  // LeftStickName
    "Stick derecho",  // RightStickName
    "Cruceta",  // DpadName
    "+ ratón",  // KeysPlusMouse
    "Arriba",  // DirectionUp
    "Abajo",  // DirectionDown
    "Izquierda",  // DirectionLeft
    "Derecha",  // DirectionRight
    "Haz clic y pulsa la nueva tecla (5 s). Clic derecho borra. Una tecla ya usada deja su entrada anterior.",  // KeysHint
    "Pulsa… %d",  // KeyCapturePrompt
    "Restaurar teclas predeterminadas",  // KeysRestoreDefaults
    "Listo",  // KeysDone
    "%s se quitó de %s.",  // KeyMovedStatus
    "%s la usa el juego y no se puede asignar.",  // KeyReservedStatus
    "Teclas predeterminadas restauradas.",  // KeysRestoredStatus
    "Espacio",  // KeySpaceName
    "Clic izq.",  // MouseLeftName
    "Clic der.",  // MouseRightName
    "Clic central",  // MouseMiddleName
    "API gráfica nativa",  // NativeBackendLabel
    "Elige Direct3D 12 o Vulkan. Requiere reiniciar el juego. Vulkan mantiene una presentación pendiente.",  // NativeBackendDescription
};

}  // namespace localization_strings
