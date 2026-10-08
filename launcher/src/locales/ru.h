#pragma once

namespace localization_strings {

inline constexpr const char* kRu[] = {
    "Naruto: Rise of a Ninja",  // WindowTitle
    "NARUTO: RISE OF A NINJA",  // AppTitle
    "Рекомпиляция для ПК (ReXGlue)",  // AppSubtitle
    "ИГРАТЬ",  // TabPlay
    "НАСТРОЙКИ",  // TabSettings
    "ФАЙЛЫ ИГРЫ",  // TabGameFiles
    "Графика",  // GroupGraphics
    "Управление",  // GroupControls
    "Монитор:",  // DisplayLabel
    "Соотношение сторон:",  // AspectLabel
    "16:9 - Стандартное (оригинал)",  // Aspect16x9
    "21:9 - UltraWide (3440x1440)",  // Aspect21x9
    "32:9 - Super UltraWide (5120x1440)",  // Aspect32x9
    "Изменяет горизонтальный угол обзора 3D-камеры без растяжения и искажения персонажей.",  // AspectTooltip
    "Разрешение:",  // ResolutionLabel
    "1280x720 - 1x (оригинал)",  // Res1x
    "2560x1440 - 2x (рекомендуется)",  // Res2x
    "3840x2160 - 3x (4K UHD)",  // Res3x
    "5120x2880 - 4x (5K UHD)",  // Res4x
    "Рендерер:",  // RendererLabel
    "Фильтрация текстур:",  // TextureFilteringLabel
    "По умолчанию",  // AnisoDefault
    "Сглаживание:",  // AntiAliasingLabel
    "Отключено",  // AaOff
    "FXAA",  // AaFxaa
    "FXAA Extreme",  // AaFxaaExtreme
    "Постобработка:",  // PostProcessingLabel
    "Билинейная (по умолчанию)",  // PostBilinear
    "CAS (резкость)",  // PostCas
    "FSR EASU (масштабирование)",  // PostFsr
    "Дизеринг",  // DitherLabel
    "Счётчик FPS",  // FpsOverlayLabel
    "Полный экран",  // FullscreenLabel
    "VSync (лимит 60 Гц)",  // VsyncLabel
    "Оставьте включённым для правильной скорости анимации и физики игры.",  // VsyncTooltip
    "Использовать SDL для геймпадов (Xbox / PlayStation)",  // SdlInputLabel
    "Геймпад:",  // ControllerStatusLabel
    "Геймпад не найден (клавиатура и мышь)",  // NoControllerDetected
    "Пропускать вступительные ролики (логотипы Ubisoft)",  // SkipIntroLabel
    "Язык:",  // LanguageLabel
    "ИГРАТЬ",  // BtnPlay
    "Рекомендуемые",  // BtnResetRecommended
    "Сохранить",  // BtnSaveSettings
    "Открыть папку игры",  // BtnOpenFolder
    "Настройки применяются при запуске игры.",  // SettingsAppliedNotice
    "Настройки сохранены.",  // SettingsSavedNotice
    "Игра готова",  // StatusReady
    "Файлы игры не найдены",  // StatusNotFound
    "Файлы игры найдены в: %s",  // FilesFoundIn
    "Поместите извлечённые файлы Xbox 360 в 'game' или выберите их папку в разделе ФАЙЛЫ ИГРЫ. ISO можно извлечь в установщике.",  // FilesMissingDesc
    "Требуются оригинальные извлечённые файлы игры Xbox 360. Извлеките ISO с помощью установщика или скопируйте файлы в 'game'.",  // FilesNotice
    "Текущий путь: %s",  // CurrentPathLabel
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
    "Настройте игру для своего ПК.",  // SettingsIntro
    "Укажите оригинальные файлы для версии на ПК.",  // FilesIntro
    "Папка игры",  // FolderHeading
    "Выберите папку с default.xex. Оригинальные файлы не перемещаются и не удаляются.",  // FolderHint
    "У вас ISO-образ?",  // SetupHeading
    "Снова запустите установщик, чтобы извлечь оригинальный ISO Xbox 360. Импорт ISO доступен в установщике.",  // SetupHint
    "Рекомендуемые настройки применены.",  // RecommendedApplied
    "Не удалось сохранить. Проверьте права на запись в папку.",  // SaveFailed
    "Исполняемый файл версии для ПК отсутствует. Переустановите её с помощью установщика.",  // ExeMissing
    "Версия для ПК не найдена",  // ExeMissingStatus
    "Файлы игры найдены",  // FilesDetected
    "Закрыть лаунчер",  // Exit
    "DLC",  // TabDlc
    "Дополнительный контент",  // DlcHeading
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
    "Язык игры:",  // GameLanguageLabel
    "Английский",  // GameLanguageEnglish
    "Французский",  // GameLanguageFrench
    "Немецкий",  // GameLanguageGerman
    "Испанский",  // GameLanguageSpanish
    "Итальянский",  // GameLanguageItalian
    "Другой (%u)",  // GameLanguageCustom
    "Применяется при запуске игры. Доступные тексты и озвучка зависят от файлов игры.",  // GameLanguageHint
    "Язык интерфейса лаунчера",  // LauncherLanguageTooltip
};

}  // namespace localization_strings
