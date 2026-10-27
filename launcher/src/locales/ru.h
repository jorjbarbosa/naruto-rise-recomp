#pragma once

namespace localization_strings {

inline constexpr const char* kRu[] = {
    "Naruto: Rise of a Ninja",  // WindowTitle
    "ИГРАТЬ",  // TabPlay
    "НАСТРОЙКИ",  // TabSettings
    "ФАЙЛЫ ИГРЫ",  // TabGameFiles
    "Качество",  // GroupQuality
    "Управление",  // GroupControls
    "Соотношение сторон",  // AspectLabel
    "16:9 - Стандартное (оригинал)",  // Aspect16x9
    "21:9 - UltraWide",  // Aspect21x9
    "32:9 - Super UltraWide",  // Aspect32x9
    "Изменяет горизонтальный угол обзора 3D-камеры без растяжения и искажения персонажей.",  // AspectDescription
    "Разрешение",  // ResolutionLabel
    "Рендерер",  // RendererLabel
    "Фильтрация текстур",  // TextureFilteringLabel
    "По умолчанию",  // AnisoDefault
    "FXAA",  // FxaaLabel
    "Отключено",  // FxaaOff
    "Обычный",  // FxaaNormal
    "Экстремальный",  // FxaaExtreme
    "Постобработка",  // PostProcessingLabel
    "Билинейная (по умолчанию)",  // PostBilinear
    "CAS (резкость)",  // PostCas
    "FSR EASU (масштабирование)",  // PostFsr
    "Дизеринг",  // DitherLabel
    "Счётчик FPS",  // FpsOverlayLabel
    "Полный экран",  // FullscreenLabel
    "VSync",  // VsyncLabel
    "Оставьте включённым для правильной скорости анимации и физики игры.",  // VsyncDescription
    "Использовать SDL для геймпадов (Xbox / PlayStation)",  // SdlInputLabel
    "Геймпад",  // ControllerLabel
    "Геймпад не найден (клавиатура и мышь)",  // NoControllerDetected
    "Пропускать вступительные ролики (логотипы Ubisoft)",  // SkipIntroLabel
    "ИГРАТЬ",  // BtnPlay
    "Рекомендуемые",  // BtnResetRecommended
    "Открыть папку игры",  // BtnOpenFolder
    "Настройки применяются при запуске игры.",  // SettingsAppliedNotice
    "Игра готова",  // StatusReady
    "Файлы игры не найдены",  // StatusNotFound
    "Поместите извлечённые файлы Xbox 360 в 'game' или выберите их папку в разделе ФАЙЛЫ ИГРЫ. ISO можно извлечь в установщике.",  // FilesMissingDesc
    "(Папка не найдена)",  // NoneDetected
    "Выбрать папку извлечённой игры...",  // BtnSelectFolder
    "Восстановить путь",  // BtnResetDefault
    "Путь по умолчанию восстановлен.",  // PathResetMsg
    "Настройки сохраняются при запуске игры или закрытии лаунчера.",  // FooterNotice
    "Игра запущена!",  // LaunchSuccessMsg
    "Не удалось запустить процесс: ",  // LaunchFailedMsg
    "Исполняемый файл игры не найден в: ",  // ExeNotFoundMsg
    "Путь обновлён: ",  // PathUpdatedMsg
    "Ваше путешествие начинается здесь.",  // PlayHeading
    "Вернитесь в Деревню Скрытого Листа.",  // PlayIntro
    "Файлы игры найдены. Можно играть.",  // ReadyDetail
    "Управление файлами игры",  // ManageFiles
    "Укажите оригинальные файлы для версии на ПК.",  // FilesIntro
    "Папка игры",  // FolderHeading
    "Выберите папку с default.xex. Оригинальные файлы не перемещаются и не удаляются.",  // FolderHint
    "У вас ISO-образ?",  // SetupHeading
    "Снова запустите установщик, чтобы извлечь оригинальный ISO Xbox 360. Импорт ISO доступен в установщике.",  // SetupHint
    "Рекомендуемые настройки применены.",  // RecommendedApplied
    "Исполняемый файл версии для ПК отсутствует. Переустановите её с помощью установщика.",  // ExeMissing
    "Версия для ПК не найдена",  // ExeMissingStatus
    "Файлы игры найдены",  // FilesDetected
    "DLC",  // TabDlc
    "Установите DLC с персонажами для Naruto: Rise of a Ninja.",  // DlcIntro
    "Установленные DLC",  // DlcInstalledHeading
    "DLC ещё не установлены.",  // DlcNoInstalled
    "Установить папку DLC",  // DlcInstallLoose
    "Выберите папку с DLC. Пакеты Xbox 360 (.live/.con/.pirs) устанавливаются при запуске игры; уже извлечённые папки копируются напрямую.",  // DlcInstallLooseHint
    "Выбрать папку DLC...",  // DlcSelectLooseFolder
    "DLC установлен.",  // DlcInstallSuccess
    "Ошибка установки DLC:",  // DlcInstallFailed
    "Пакеты DLC подготовлены. Они будут установлены при следующем запуске игры.",  // DlcStfsStaged
    "Открыть папку DLC",  // DlcOpenContentFolder
    "Подготовить папки DLC",  // DlcPrepareFolders
    "Папки DLC готовы.",  // DlcFoldersReady
    "Naruto: Rise of a Ninja - Установка",  // InstWindowTitle
    "Добро пожаловать в установщик Naruto: Rise of a Ninja для ПК",  // InstWelcomeTitle
    "Этот мастер устанавливает версию для ПК и импортирует оригинальные файлы игры Xbox 360.",  // InstWelcomeIntro
    "1. Выберите папку установки",  // InstWelcomeStep1
    "2. Выберите ISO-образ Naruto: Rise of a Ninja",  // InstWelcomeStep2
    "3. Файлы игры извлекаются автоматически",  // InstWelcomeStep3
    "4. Создаются ярлыки (по желанию)",  // InstWelcomeStep4
    "5. Запускайте игру через лаунчер",  // InstWelcomeStep5
    "Вы должны владеть оригинальной игрой. Файлы игры не распространяются вместе с этой версией.",  // InstWelcomeLegal
    "Файлы установки не найдены. Запустите установщик из папки дистрибутива или используйте собранный установщик.",  // InstPayloadMissing
    "Папка установки",  // InstDestTitle
    "Файлы версии для ПК будут скопированы в эту папку.",  // InstDestHint
    "Обзор...",  // InstBrowse
    "Далее",  // InstNext
    "Назад",  // InstBack
    "УСТАНОВИТЬ",  // InstInstall
    "Готово",  // InstFinish
    "ISO-образ игры",  // InstIsoTitle
    "Выберите оригинальный ISO Xbox 360 (.iso).",  // InstIsoHint
    "Выберите ISO Naruto: Rise of a Ninja",  // InstIsoDialog
    "Проверка ISO-образа...",  // InstIsoChecking
    "Допустимый ISO Xbox 360: %u файлов (%.2f ГБ)",  // InstIsoValid
    "Выбранный файл не является допустимым ISO Xbox 360.",  // InstIsoInvalid
    "default.xex не найден в ISO-образе.",  // InstIsoNoXex
    "Пропустить извлечение (файлы игры уже находятся в папке назначения)",  // InstIsoSkipCheck
    "Выберите допустимый ISO или отметьте 'Пропустить извлечение', если файлы игры уже на месте.",  // InstIsoRequired
    "Укажите допустимую папку установки.",  // InstInvalidDest
    "Установка",  // InstProgressTitle
    "Подготовка к установке...",  // InstPreparing
    "Удаление предыдущих файлов игры...",  // InstRemovingOld
    "Извлечение ISO-образа (это может занять несколько минут)...",  // InstExtractingIso
    "Копирование файлов версии для ПК...",  // InstCopyingFiles
    "Сохранение настроек...",  // InstWritingConfig
    "Регистрация в списке программ...",  // InstRegistering
    "Установка завершена!",  // InstComplete
    "Установка отменена.",  // InstCanceled
    "Ошибка установки",  // InstFailedTitle
    "Установка завершена",  // InstFinishTitle
    "Установлено в: %s",  // InstInstalledTo
    "Файлы игры в: %s",  // InstGameFilesAt
    "Создать ярлык на рабочем столе",  // InstShortcutDesktop
    "Создать ярлык в меню «Пуск»",  // InstShortcutStartMenu
    "Запустить лаунчер Naruto сейчас",  // InstLaunchNow
    "Naruto: Rise of a Ninja - Удаление",  // UnWindowTitle
    "Удаляет файлы версии для ПК, ярлыки и запись в списке программ.",  // UnIntro
    "Удалить (сохранить файлы игры)",  // UnKeepFiles
    "Удалить всё",  // UnRemoveAll
    "Отмена",  // UnCancel
    "Удаление завершено. Некоторые файлы могут оставаться до закрытия этого окна.",  // UnDone
    "Ошибка удаления: %s",  // UnFailed
    "Игра",  // GroupGame
    "Язык игры",  // GameLanguageLabel
    "Английский",  // GameLanguageEnglish
    "Французский",  // GameLanguageFrench
    "Немецкий",  // GameLanguageGerman
    "Испанский",  // GameLanguageSpanish
    "Итальянский",  // GameLanguageItalian
    "Другой (%u)",  // GameLanguageCustom
    "Применяется при запуске игры. Переводы из папки mods появляются здесь и заменяют текст только после выбора. Доступная озвучка зависит от файлов игры.",  // GameLanguageDescription
    "Язык интерфейса лаунчера",  // LauncherLanguageTooltip
    "Xenos (эмуляция GPU)",  // RendererXenos
    "Нативный",  // RendererNative
    "Графический API игры: Direct3D 12 или Vulkan. Выбор применяется при следующем запуске.",  // RendererDescription
    "Недоступно с нативным рендерером. Чтобы включить, выберите Xenos в пункте «Рендерер».",  // XenosOnlyReason
    "MSAA",  // MsaaLabel
    "4x (оригинал)",  // MsaaOriginal
    "Мультисэмплинг 3D-сцен игры. Для 8x нужна более мощная видеокарта.",  // MsaaDescription
    "Сглаживание прозрачности",  // TransparencyAaLabel
    "Сглаживает края листвы, заборов и других вырезанных текстур (alpha to coverage).",  // TransparencyAaDescription
    "Частота кадров",  // FrameRateLabel
    "30 — исходная частота игры. Выше логика игры сохраняет свой темп, а промежуточные кадры интерполируются. С VSync частота не выше частоты монитора; высокие значения требуют быстрого процессора.",  // FrameRateDescription
    "Яркость",  // BrightnessLabel
    "50 — исходная яркость игры.",  // BrightnessDescription
    "Задержка кадров",  // FrameLatencyLabel
    "1 (минимальная)",  // FrameLatencyLowest
    "Кадры в очереди перед выводом на экран. 1 даёт наименьшую задержку ввода; 2 или 3 могут сгладить неровное время кадра.",  // FrameLatencyDescription
    "Тройная буферизация",  // TripleBufferingLabel
    "Три буфера вывода вместо двух.",  // TripleBufferingDescription
    "Синхронизирует вывод с экраном. С нативным рендерером скорость игры не зависит от VSync.",  // VsyncNativeDescription
    "Видеокарта",  // GraphicsDeviceLabel
    "Автоматически",  // GraphicsDeviceAuto
    "Видеокарта, на которой работает игра. «Автоматически» оставляет выбор рендереру. На ноутбуках с двумя видеокартами выберите дискретную (NVIDIA или AMD).",  // GraphicsDeviceDescription
    "Экран",  // GroupDisplay
    "Дополнительно",  // GroupAdvanced
    "только Xenos",  // XenosOnlyTag
    "только нативный",  // NativeOnlyTag
    "Только с нативным рендерером. Чтобы включить, выберите «Нативный» в пункте «Рендерер».",  // NativeOnlyReason
    "Разрешение, в котором рисуется игра, кратное исходным 1280×720. Чем выше значение, тем чётче изображение и тем мощнее нужна видеокарта.",  // ResolutionDescription
    "Анизотропная фильтрация сохраняет чёткость текстур на поверхностях под углом, например на полу и дорогах.",  // TextureFilteringDescription
    "Сглаживает зубчатые края, фильтруя итоговое изображение. «Экстремальный» сглаживает сильнее и может размыть мелкие детали.",  // FxaaDescription
    "Как изображение подгоняется под экран. CAS повышает резкость, FSR масштабирует с повышением резкости, а билинейная фильтрация оставляет картинку мягкой.",  // PostProcessingDescription
    "Добавляет мелкий шум, скрывающий цветовые полосы в градиентах.",  // DitherDescription
    "Показывает частоту и время кадра в углу экрана. F1 включает и выключает его в игре.",  // FpsOverlayDescription
    "Запускает игру в полноэкранном режиме. Если выключено, игра работает в окне.",  // FullscreenDescription
    "Пропускает логотипы при запуске игры. Вступительный ролик сохраняется.",  // SkipIntroDescription
    "Читает геймпады через SDL, который распознаёт геймпады Xbox, PlayStation и другие. Если выключено, игра использует XInput, который распознаёт только Xbox-совместимые геймпады.",  // SdlInputDescription
    "Подключённый сейчас геймпад, под именем, которое ему даёт SDL.",  // ControllerDescription
    "Клавиатура и мышь",  // KeyboardMouseLabel
    "Клавиатура и мышь заменяют геймпад Xbox и работают вместе с подключённым геймпадом. Если мышь управляет камерой, курсор остаётся в окне игры, пока оно активно; Alt+Tab освобождает его. Игра по-прежнему показывает кнопки геймпада.",  // KeyboardMouseDescription
    "Мышь управляет камерой",  // MouseCameraLabel
    "Камера (правый стик) двигается мышью. Если выключено, камера двигается только клавишами правого стика.",  // MouseCameraDescription
    "Чувствительность",  // MouseSensitivityLabel
    "Как быстро камера следует за мышью. 100% — значение по умолчанию.",  // MouseSensitivityDescription
    "Клавиши",  // KeysLabel
    "Клавиши и кнопки мыши для каждой кнопки геймпада, по две на кнопку.",  // KeysDescription
    "Назначить клавиши…",  // MapKeysButton
    "Назначение клавиш",  // MapKeysTitle
    "Управление › Клавиатура и мышь",  // MapKeysPath
    "Кнопки",  // KeysButtonsHeading
    "Левый стик",  // LeftStickName
    "Правый стик",  // RightStickName
    "Крестовина",  // DpadName
    "+ мышь",  // KeysPlusMouse
    "Вверх",  // DirectionUp
    "Вниз",  // DirectionDown
    "Влево",  // DirectionLeft
    "Вправо",  // DirectionRight
    "Нажмите на поле, затем новую клавишу (5 с). Правый щелчок очищает. Занятая клавиша снимается с прежней кнопки.",  // KeysHint
    "Нажмите… %d",  // KeyCapturePrompt
    "Вернуть клавиши по умолчанию",  // KeysRestoreDefaults
    "Готово",  // KeysDone
    "%s снята с кнопки %s.",  // KeyMovedStatus
    "%s используется игрой и не может быть назначена.",  // KeyReservedStatus
    "Клавиши по умолчанию восстановлены.",  // KeysRestoredStatus
    "Пробел",  // KeySpaceName
    "ЛКМ",  // MouseLeftName
    "ПКМ",  // MouseRightName
    "СКМ",  // MouseMiddleName
    "Графический API",  // NativeBackendLabel
    "Выберите Direct3D 12 или Vulkan. Требуется перезапуск игры. Vulkan ставит в очередь один кадр.",  // NativeBackendDescription
    "Проверить обновления",  // UpdatesCheck
    "Проверка обновлений…",  // UpdatesChecking
    "У вас актуальная версия.",  // UpdatesCurrent
    "Не удалось проверить. Повторите позже.",  // UpdatesFailed
    "Не удалось сравнить установленную версию.",  // UpdatesUnknownVersion
    "Доступна новая версия: %s",  // UpdatesAvailable
    "Что нового",  // UpdatesNotes
    "Скачать обновление",  // UpdatesDownload
    "Установщик для Windows",  // UpdatesInstaller
    "Портативный ZIP",  // UpdatesPortable
    "Позже",  // UpdatesLater
    "Пропустить эту версию",  // UpdatesIgnore
    "Не удалось открыть браузер. Попробуйте снова.",  // UpdatesBrowserFailed
    "Открывает загрузку в браузере. Закройте игру и лаунчер перед обновлением.",  // UpdatesDownloadHint
    "Подождите. Проверка доступна раз в минуту.",  // UpdatesCheckCooldown
    "Скачать вручную",  // UpdatesManualDownload
    "Загрузка обновления…",  // UpdatesDownloading
    "Проверка загрузки…",  // UpdatesVerifying
    "Отмена…",  // UpdatesCancelling
    "Отменить",  // UpdatesCancel
    "Загрузка отменена.",  // UpdatesCancelled
    "Загрузка проверена. Лаунчер закроется, и установщик обновит эту папку:",  // UpdatesReadyToInstall
    "Установить и закрыть лаунчер",  // UpdatesInstallClose
    "Ошибка загрузки. Проверьте подключение и повторите попытку.",  // UpdatesDownloadFailed
    "Файл не прошёл проверку. Загрузите его снова.",  // UpdatesIntegrityFailed
    "Не удалось подтвердить обновление. Проверьте снова или откройте страницу выпуска через «Что нового».",  // UpdatesMetadataFailed
    "Не удалось сохранить файл. Проверьте свободное место и права доступа к папке.",  // UpdatesStorageFailed
    "Закройте игру и другие лаунчеры из этой папки, затем повторите установку.",  // UpdatesCloseGame
    "Не удалось запустить установщик. Повторите попытку или откройте страницу выпуска через «Что нового».",  // UpdatesInstallFailed
    "Не удалось сохранить настройки. Исправьте права доступа к файлу перед установкой.",  // UpdatesSaveFailed
    "Регистрация установки изменилась. Закройте и снова откройте лаунчер, затем повторите попытку.",  // UpdatesNotInstalled
    "Скачать и установить",  // UpdatesDownloadInstall
    "Запустить установщик",  // UpdatesLaunchInstaller
    "Установщик будет скачан и проверен. Затем мы снова запросим подтверждение, прежде чем закрыть лаунчер и открыть мастер установки для обновления игры.",  // UpdatesConfirmDownload
    "Установщик скачан и проверен. Запустить его сейчас? Настройки будут сохранены, а лаунчер закроется. Мастер установки проведёт вас через обновление. Закройте игру перед продолжением.",  // UpdatesConfirmInstall
    "Это портативная копия. Установщик зарегистрирует её как приложение Windows в той же папке. Этот процесс не использует ZIP.",  // UpdatesPortableConversion
    "Папка установки",  // UpdatesDestination
    "Продолжить загрузку?",  // UpdatesContinue
    "Да",  // UpdatesYes
    "Нет",  // UpdatesNo
    "Загрузка готова. Нажмите «Скачать и установить», чтобы открыть установщик.",  // UpdatesDownloadReady
    "Для этой версии нет установщика Windows. См. «Что нового».",  // UpdatesNoInstaller
    "Отменить загрузку",  // UpdatesCancelDownload
    "В этой папке нет полного порта игры. Используйте лаунчер в папке с narutorise.exe и narutorise_launcher.exe.",  // UpdatesMissingPortFiles
};

}  // namespace localization_strings
