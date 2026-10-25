// Additional vocabulary of the setup screen and the in-game menu.
#include "menu/text.h"
#include <array>
namespace narutorise::menu {
namespace {
struct Entry { std::string_view key; std::array<const char*, 7> values; };
constexpr Entry strings[] = {
 {"setup-play", {"PLAY", "JOGAR", "JOUER", "SPIELEN", "JUGAR", "GIOCA", "ИГРАТЬ"}},
 {"setup-choose-hint", {"Select your ISO or game folder to begin.", "Selecione sua ISO ou a pasta do jogo para começar.", "Sélectionnez votre ISO ou le dossier du jeu pour commencer.", "Wähle deine ISO oder den Spielordner, um zu beginnen.", "Selecciona tu ISO o la carpeta del juego para comenzar.", "Seleziona la tua ISO o la cartella del gioco per iniziare.", "Выберите ISO или папку игры, чтобы начать."}},
 {"setup-ready", {"Game ready", "Jogo pronto", "Jeu prêt", "Spiel bereit", "Juego listo", "Gioco pronto", "Игра готова"}},
 {"setup-no-game", {"No game selected", "Nenhum jogo selecionado", "Aucun jeu sélectionné", "Kein Spiel ausgewählt", "Ningún juego seleccionado", "Nessun gioco selezionato", "Игра не выбрана"}},
 {"setup-pad-hints", {"A  Confirm     D-pad  Navigate", "A  Confirmar     Direcional  Navegar", "A  Confirmer     Croix  Naviguer", "A  Bestätigen     Steuerkreuz  Navigieren", "A  Confirmar     Cruceta  Navegar", "A  Conferma     Croce  Naviga", "A  Подтвердить     Крестовина  Навигация"}},
 {"setup-key-hints", {"Enter  Confirm     Arrows  Navigate", "Enter  Confirmar     Setas  Navegar", "Entrée  Confirmer     Flèches  Naviguer", "Enter  Bestätigen     Pfeiltasten  Navigieren", "Enter  Confirmar     Flechas  Navegar", "Invio  Conferma     Frecce  Naviga", "Enter  Подтвердить     Стрелки  Навигация"}},
 {"incomplete", {"MISSING GAME DATA", "DADOS DO JOGO INCOMPLETOS", "DONNÉES DU JEU MANQUANTES", "SPIELDATEN FEHLEN", "FALTAN DATOS DEL JUEGO", "DATI DI GIOCO MANCANTI", "НЕ ХВАТАЕТ ДАННЫХ ИГРЫ"}},
 {"incomplete-detail", {"Select the ISO or the full extracted game folder", "Selecione o ISO ou a pasta completa do jogo extraído", "Choisissez l’ISO ou le dossier complet du jeu extrait", "ISO oder vollständigen entpackten Spielordner wählen", "Selecciona el ISO o la carpeta completa del juego extraído", "Seleziona l’ISO o la cartella completa del gioco estratto", "Выберите ISO или полную папку распакованной игры"}},
 {"display-section", {"Display", "Exibição", "Affichage", "Anzeige", "Pantalla", "Schermo", "Экран"}},
 {"add-dlc", {"Add DLC...", "Adicionar DLC...", "Ajouter un DLC...", "DLC hinzufügen...", "Añadir DLC...", "Aggiungi DLC...", "Добавить DLC..."}},
 {"installed", {"Installed", "Instalado", "Installé", "Installiert", "Instalado", "Installato", "Установлено"}},
 {"installed-list", {"Installed", "Instalados", "Installés", "Installiert", "Instalados", "Installati", "Установлено"}},
 {"mapping-pad-hints", {"A  Map     X  Clear     B  Back", "A  Mapear     X  Limpar     B  Voltar", "A  Assigner     X  Effacer     B  Retour", "A  Belegen     X  Löschen     B  Zurück", "A  Asignar     X  Borrar     B  Volver", "A  Assegna     X  Cancella     B  Indietro", "A  Назначить     X  Очистить     B  Назад"}},
 {"mapping-key-hints", {"Enter  Map     Delete  Clear     Esc  Back", "Enter  Mapear     Delete  Limpar     Esc  Voltar", "Entrée  Assigner     Suppr  Effacer     Échap  Retour", "Enter  Belegen     Entf  Löschen     Esc  Zurück", "Enter  Asignar     Supr  Borrar     Esc  Volver", "Invio  Assegna     Canc  Cancella     Esc  Indietro", "Enter  Назначить     Delete  Очистить     Esc  Назад"}},
 {"install-on-restart", {"Installs on restart", "Instala ao reiniciar", "Installation au redémarrage", "Installation beim Neustart", "Se instala al reiniciar", "Installa al riavvio", "Установка после перезапуска"}},
 {"not-connected", {"Not connected", "Não conectado", "Non connecté", "Nicht verbunden", "No conectado", "Non connesso", "Не подключён"}},
 {"shader-count", {"Compiling %u shaders", "Compilando %u shaders", "Compilation de %u shaders", "%u Shader werden kompiliert", "Compilando %u shaders", "Compilazione di %u shader", "Компиляция %u шейдеров"}},
 {"list-pad-hints", {"A  Select     B  Back", "A  Selecionar     B  Voltar", "A  Choisir     B  Retour", "A  Wählen     B  Zurück", "A  Elegir     B  Volver", "A  Seleziona     B  Indietro", "A  Выбрать     B  Назад"}},
 {"list-key-hints", {"Enter  Select     Esc  Back", "Enter  Selecionar     Esc  Voltar", "Entrée  Choisir     Échap  Retour", "Enter  Wählen     Esc  Zurück", "Enter  Elegir     Esc  Volver", "Invio  Seleziona     Esc  Indietro", "Enter  Выбрать     Esc  Назад"}},
 {"close-pad", {"B  Close", "B  Fechar", "B  Fermer", "B  Schließen", "B  Cerrar", "B  Chiudi", "B  Закрыть"}},
 {"close-key", {"Esc  Close", "Esc  Fechar", "Échap  Fermer", "Esc  Schließen", "Esc  Cerrar", "Esc  Chiudi", "Esc  Закрыть"}},
 {"update-available", {"New version available", "Nova versão disponível", "Nouvelle version disponible", "Neue Version verfügbar", "Nueva versión disponible", "Nuova versione disponibile", "Доступна новая версия"}},
 {"update-detail", {"%s • see Settings › Advanced", "%s • veja em Configurações › Avançado", "%s • voir Paramètres › Avancé", "%s • siehe Einstellungen › Erweitert", "%s • ver Ajustes › Avanzado", "%s • vedi Impostazioni › Avanzate", "%s • Настройки › Дополнительно"}},
 {"panel-pad-hints", {"A  Select     ← →  Adjust     LB RB  Tabs     B  Back", "A  Selecionar     ← →  Ajustar     LB RB  Abas     B  Voltar", "A  Choisir     ← →  Régler     LB RB  Onglets     B  Retour", "A  Wählen     ← →  Ändern     LB RB  Reiter     B  Zurück", "A  Elegir     ← →  Ajustar     LB RB  Pestañas     B  Volver", "A  Seleziona     ← →  Regola     LB RB  Schede     B  Indietro", "A  Выбрать     ← →  Изменить     LB RB  Вкладки     B  Назад"}},
 {"panel-key-hints", {"Enter  Select     ← →  Adjust     PgUp PgDn  Tabs     Esc  Back", "Enter  Selecionar     ← →  Ajustar     PgUp PgDn  Abas     Esc  Voltar", "Entrée  Choisir     ← →  Régler     PgUp PgDn  Onglets     Échap  Retour", "Enter  Wählen     ← →  Ändern     PgUp PgDn  Reiter     Esc  Zurück", "Enter  Elegir     ← →  Ajustar     PgUp PgDn  Pestañas     Esc  Volver", "Invio  Seleziona     ← →  Regola     PgUp PgDn  Schede     Esc  Indietro", "Enter  Выбрать     ← →  Изменить     PgUp PgDn  Вкладки     Esc  Назад"}},
 {"video", {"Video", "Vídeo", "Vidéo", "Video", "Vídeo", "Video", "Видео"}},
 {"keyboard-mouse", {"Keyboard and mouse", "Teclado e mouse", "Clavier et souris", "Tastatur und Maus", "Teclado y ratón", "Tastiera e mouse", "Клавиатура и мышь"}},
 {"input-section", {"Controller", "Controle", "Manette", "Controller", "Mando", "Controller", "Геймпад"}},
 {"menu-section", {"Menu", "Menu", "Menu", "Menü", "Menú", "Menu", "Меню"}},
 {"graphics-api", {"Graphics API", "API gráfica", "API graphique", "Grafik-API", "API gráfica", "API grafica", "Графический API"}},
 {"input-api", {"Input backend", "Backend de entrada", "Mode de saisie", "Eingabemodus", "Modo de entrada", "Sistema di input", "Система ввода"}},
 {"mouse-camera", {"Mouse camera", "Câmera com mouse", "Caméra à la souris", "Mauskamera", "Cámara con ratón", "Camera con mouse", "Камера мышью"}},
 {"sensitivity", {"Sensitivity", "Sensibilidade", "Sensibilité", "Empfindlichkeit", "Sensibilidad", "Sensibilità", "Чувствительность"}},
 {"key-mapping", {"Key mapping", "Mapear teclas", "Touches", "Tastenbelegung", "Asignar teclas", "Mappa tasti", "Назначение клавиш"}},
 {"skip-videos", {"Skip intro videos", "Pular vídeos iniciais", "Passer les vidéos", "Introvideos überspringen", "Saltar vídeos iniciales", "Salta video iniziali", "Пропускать заставки"}},
 {"update-hint", {"Check for a new release and open its page.", "Verificar nova versão e abrir sua página.", "Rechercher une nouvelle version et ouvrir sa page.", "Nach einer neuen Version suchen und ihre Seite öffnen.", "Buscar una nueva versión y abrir su página.", "Cerca una nuova versione e apri la sua pagina.", "Проверить новую версию и открыть её страницу."}},
 {"open-release", {"Open release page", "Abrir página da versão", "Ouvrir la page de la version", "Versionsseite öffnen", "Abrir página de la versión", "Apri pagina della versione", "Открыть страницу версии"}},
 {"shortcut-format", {"Press %s or %s to open the menu", "Pressione  %s  ou  %s  para abrir o menu", "Appuyez sur %s ou %s pour ouvrir le menu", "%s oder %s drücken, um das Menü zu öffnen", "Pulsa %s o %s para abrir el menú", "Premi %s o %s per aprire il menu", "Нажмите %s или %s, чтобы открыть меню"}},
 {"working", {"Working. Please wait.", "Aguarde a conclusão.", "Opération en cours. Patientez.", "Vorgang läuft. Bitte warten.", "Operación en curso. Espera.", "Operazione in corso. Attendi.", "Операция выполняется. Подождите."}},
 {"game-filter", {"Xbox 360 game (*.iso; default.xex)", "Jogo do Xbox 360 (*.iso; default.xex)", "Jeu Xbox 360 (*.iso; default.xex)", "Xbox-360-Spiel (*.iso; default.xex)", "Juego de Xbox 360 (*.iso; default.xex)", "Gioco Xbox 360 (*.iso; default.xex)", "Игра Xbox 360 (*.iso; default.xex)"}},
 {"all-files", {"All files", "Todos os arquivos", "Tous les fichiers", "Alle Dateien", "Todos los archivos", "Tutti i file", "Все файлы"}},
 {"choose", {"CHOOSE THE GAME", "ESCOLHER O JOGO", "CHOISIR LE JEU", "SPIEL WÄHLEN", "ELEGIR EL JUEGO", "SCEGLI IL GIOCO", "ВЫБРАТЬ ИГРУ"}},
 {"quit", {"QUIT", "SAIR", "QUITTER", "BEENDEN", "SALIR", "ESCI", "ВЫХОД"}},
 {"pc", {"PORT FOR PC", "PORT PARA PC", "PORTAGE PC", "PC-PORTIERUNG", "VERSIÓN PARA PC", "VERSIONE PC", "ПОРТ ДЛЯ ПК"}},
 {"no-game", {"NO GAME", "SEM JOGO", "AUCUN JEU", "KEIN SPIEL", "SIN JUEGO", "NESSUN GIOCO", "НЕТ ИГРЫ"}},
 {"game-hint", {"Xbox 360 .iso image or extracted default.xex", "Imagem .iso do Xbox 360 ou default.xex extraído", "Image .iso Xbox 360 ou default.xex extrait", "Xbox-360-ISO oder entpackte default.xex", "Imagen .iso de Xbox 360 o default.xex extraído", "Immagine .iso Xbox 360 o default.xex estratto", "Образ .iso Xbox 360 или извлечённый default.xex"}},
 {"missing", {"GAME NOT FOUND", "JOGO NÃO ENCONTRADO", "JEU INTROUVABLE", "SPIEL NICHT GEFUNDEN", "JUEGO NO ENCONTRADO", "GIOCO NON TROVATO", "ИГРА НЕ НАЙДЕНА"}},
 {"missing-detail", {"The game file or folder was moved or deleted", "O arquivo ou pasta do jogo foi movido ou apagado", "Le fichier ou dossier du jeu a été déplacé ou supprimé", "Die Spieldatei wurde verschoben oder gelöscht", "El archivo del juego se movió o eliminó", "Il file del gioco è stato spostato o eliminato", "Файл игры перемещён или удалён"}},
 {"unsupported", {"Unsupported version", "Versão não suportada", "Version non prise en charge", "Nicht unterstützte Version", "Versión no compatible", "Versione non supportata", "Неподдерживаемая версия"}},
 {"unsupported-detail", {"This port requires another version of default.xex. Choose the supported game image.", "Este port exige outra versão de default.xex. Escolha a imagem compatível do jogo.", "Ce portage nécessite une autre version de default.xex. Choisissez une image compatible.", "Dieser Port benötigt eine andere default.xex-Version. Bitte ein unterstütztes Image wählen.", "Este port requiere otra versión de default.xex. Elige una imagen compatible.", "Questo port richiede un'altra versione di default.xex. Scegli un'immagine compatibile.", "Для порта нужна другая версия default.xex. Выберите совместимый образ игры."}},
 {"unrecognized", {"File not recognized", "Arquivo não reconhecido", "Fichier non reconnu", "Datei nicht erkannt", "Archivo no reconocido", "File non riconosciuto", "Файл не распознан"}},
 {"unreadable", {"Could not read the file", "Não foi possível ler o arquivo", "Impossible de lire le fichier", "Datei konnte nicht gelesen werden", "No se pudo leer el archivo", "Impossibile leggere il file", "Не удалось прочитать файл"}},
 {"start-failed", {"THE LAST START FAILED", "A ÚLTIMA ABERTURA FALHOU", "LE DERNIER DÉMARRAGE A ÉCHOUÉ", "LETZTER START FEHLGESCHLAGEN", "EL ÚLTIMO INICIO FALLÓ", "L'ULTIMO AVVIO È FALLITO", "ПОСЛЕДНИЙ ЗАПУСК НЕ УДАЛСЯ"}},
 {"start-detail", {"Try another graphics API or card in Settings > Display", "Tente outra API ou placa em Configurações > Tela", "Essayez une autre API ou carte dans Paramètres > Affichage", "Andere API oder Grafikkarte unter Einstellungen > Anzeige wählen", "Prueba otra API o tarjeta en Ajustes > Pantalla", "Prova un'altra API o scheda in Impostazioni > Schermo", "Выберите другой API или видеокарту в настройках экрана"}},
 {"image", {"Xbox 360 · Disc image", "Xbox 360 · Imagem de disco", "Xbox 360 · Image disque", "Xbox 360 · Disc-Image", "Xbox 360 · Imagen de disco", "Xbox 360 · Immagine disco", "Xbox 360 · Образ диска"}},
 {"folder", {"Xbox 360 · Extracted folder", "Xbox 360 · Pasta extraída", "Xbox 360 · Dossier extrait", "Xbox 360 · Entpackter Ordner", "Xbox 360 · Carpeta extraída", "Xbox 360 · Cartella estratta", "Xbox 360 · Извлечённая папка"}},
 {"cancel", {"Cancel", "Cancelar", "Annuler", "Abbrechen", "Cancelar", "Annulla", "Отмена"}},
 {"choose-another", {"Choose another", "Escolher outro", "Choisir un autre", "Andere wählen", "Elegir otro", "Scegli un altro", "Выбрать другой"}},
 {"checking", {"Checking the game", "Verificando o jogo", "Vérification du jeu", "Spiel wird geprüft", "Verificando el juego", "Verifica del gioco", "Проверка игры"}},
 {"wait", {"Reading default.xex. Please wait.", "Lendo default.xex. Aguarde.", "Lecture de default.xex. Patientez.", "default.xex wird gelesen. Bitte warten.", "Leyendo default.xex. Espera.", "Lettura di default.xex. Attendi.", "Чтение default.xex. Подождите."}},
 {"save-failed", {"Could not save settings", "Não foi possível salvar", "Impossible d'enregistrer", "Speichern fehlgeschlagen", "No se pudo guardar", "Salvataggio non riuscito", "Не удалось сохранить"}},
 {"applying-settings", {"Applying settings…", "Aplicando configurações…", "Application des paramètres…", "Einstellungen werden angewendet…", "Aplicando ajustes…", "Applicazione delle impostazioni…", "Применение настроек…"}},
 {"apply-settings-failed", {"Could not apply this setting. The previous setting was kept.", "Não foi possível aplicar esta opção. A configuração anterior foi mantida.", "Impossible d'appliquer ce paramètre. Le précédent a été conservé.", "Diese Einstellung konnte nicht angewendet werden. Die vorherige wurde beibehalten.", "No se pudo aplicar este ajuste. Se mantuvo el anterior.", "Impossibile applicare questa impostazione. È stata mantenuta la precedente.", "Не удалось применить настройку. Сохранено предыдущее значение."}},
 {"picker-failed", {"Could not open the file picker", "Não foi possível abrir o seletor", "Impossible d'ouvrir le sélecteur", "Dateiauswahl konnte nicht geöffnet werden", "No se pudo abrir el selector", "Impossibile aprire il selettore", "Не удалось открыть выбор файла"}},
 {"back", {"Back", "Voltar", "Retour", "Zurück", "Volver", "Indietro", "Назад"}},
 {"on", {"On", "Ligado", "Activé", "Ein", "Activado", "Attivo", "Вкл."}},
 {"off", {"Off", "Desligado", "Désactivé", "Aus", "Desactivado", "Disattivo", "Выкл."}},
 {"restart", {"RESTART", "REINICIA", "REDÉMARRAGE", "NEUSTART", "REINICIAR", "RIAVVIO", "ПЕРЕЗАПУСК"}},
 {"restart-required", {"Restart to apply?", "Reiniciar para aplicar?", "Redémarrer pour appliquer ?", "Zum Anwenden neu starten?", "¿Reiniciar para aplicar?", "Riavviare per applicare?", "Перезапустить для применения?"}},
 {"restart-now", {"Restart now", "Reiniciar agora", "Redémarrer maintenant", "Jetzt neu starten", "Reiniciar ahora", "Riavvia ora", "Перезапустить сейчас"}},
 {"later", {"Later", "Depois", "Plus tard", "Später", "Después", "Più tardi", "Позже"}},
 {"language", {"Menu language", "Idioma do menu", "Langue du menu", "Menüsprache", "Idioma del menú", "Lingua del menu", "Язык меню"}},
 {"changed", {"Saved. Restart to apply the marked options.", "Salvo. Reinicie para aplicar as opções marcadas.", "Enregistré. Redémarrez pour appliquer les options marquées.", "Gespeichert. Markierte Optionen erfordern einen Neustart.", "Guardado. Reinicia para aplicar las opciones marcadas.", "Salvato. Riavvia per applicare le opzioni indicate.", "Сохранено. Для отмеченных настроек нужен перезапуск."}},
 {"exit-game", {"Exit game", "Sair do jogo", "Quitter le jeu", "Spiel beenden", "Salir del juego", "Esci dal gioco", "Выйти из игры"}},
 {"exit-confirm", {"Progress since the last save will be lost.", "O progresso desde o último salvamento será perdido.", "La progression depuis la dernière sauvegarde sera perdue.", "Fortschritt seit dem letzten Speichern geht verloren.", "Se perderá el progreso desde el último guardado.", "I progressi dall'ultimo salvataggio andranno persi.", "Прогресс с последнего сохранения будет потерян."}},
 {"shortcut", {"Create desktop shortcut", "Criar atalho na área de trabalho", "Créer un raccourci sur le bureau", "Desktop-Verknüpfung erstellen", "Crear acceso directo en el escritorio", "Crea collegamento sul desktop", "Создать ярлык на рабочем столе"}},
 {"shortcut-done", {"Desktop shortcut created", "Atalho criado na área de trabalho", "Raccourci créé sur le bureau", "Desktop-Verknüpfung erstellt", "Acceso directo creado", "Collegamento creato sul desktop", "Ярлык создан на рабочем столе"}},
 {"shortcut-hint", {"F1 or Back + Start: game menu", "F1 ou Back + Start: menu do jogo", "F1 ou Back + Start : menu du jeu", "F1 oder Back + Start: Spielmenü", "F1 o Back + Start: menú del juego", "F1 o Back + Start: menu di gioco", "F1 или Back + Start: меню игры"}},
 {"shaders", {"Compiling shaders", "Compilando shaders", "Compilation des shaders", "Shader werden kompiliert", "Compilando shaders", "Compilazione shader", "Компиляция шейдеров"}},
 {"shader-option", {"Show shader indicator", "Mostrar indicador de shaders", "Afficher la compilation des shaders", "Shader-Anzeige einblenden", "Mostrar indicador de shaders", "Mostra indicatore shader", "Индикатор компиляции шейдеров"}},
 {"menu-key", {"Menu shortcut", "Atalho do menu", "Raccourci du menu", "Menü-Tastenkürzel", "Atajo del menú", "Scorciatoia del menu", "Клавиша меню"}},
 {"pad-shortcut", {"Controller shortcut", "Atalho do controle", "Raccourci manette", "Controller-Tastenkürzel", "Atajo del mando", "Scorciatoia controller", "Комбинация геймпада"}},
};
}
const char* Text(std::string_view key) {
  extern const char* ControlsText(std::string_view key);
  if (auto* translated = ControlsText(key)) return translated;
  if (key == "play") return Tr(TextId::BtnPlay);
  if (key == "settings") return Tr(TextId::TabSettings);
  if (key == "ready") return Tr(TextId::StatusReady);
  if (key == "dlc") return Tr(TextId::TabDlc);
  const size_t language = size_t(Localization::GetLanguage());
  for (const auto& entry : strings) if (key == entry.key) return entry.values[language < 7 ? language : 0];
  return "";
}
int TextSystem::TranslateString(Rml::String& translated, const Rml::String& input) {
  if (!input.empty() && input[0] == '@') {
    translated = Text(std::string_view(input).substr(1));
    return 1;
  }
  translated = input;
  return 0;
}
}  // namespace narutorise::menu
