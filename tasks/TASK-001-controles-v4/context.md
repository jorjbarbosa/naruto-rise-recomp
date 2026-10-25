# Contexto — TASK-001

## Objetivo
Implementar o redesenho de controles solicitado em 2026-10-25, usando o documento atual do FigCraft como referência visual e conectando quatro portas ao sistema real de entrada e persistência. Plano aprovado pelo usuário (“aprovado”) em 2026-10-25; implementação executada na branch feat/no-launcher. Estado e evidências finais em progress/tests/handoff.

## Fontes e referência visual
- Projeto: `C:/Users/Jorge/dev/naruto-rise-dev`.
- FigCraft: `C:/Users/Jorge/Documents/f0-menus.figcraft`, aberto e consultado pelo MCP em 2026-10-25; documento sem alterações pendentes.
- Página: **F0 v4 · estrutura Dusklight, estética própria**, ID `4294968293`.
- Fluxo confirmado pelo MCP: **Controles · 4 jogadores**, início `8589934593`. Sua descrição exige captura na própria linha, Esc para cancelar, Del para remover e navegação suspensa.
- Foram renderizadas e inspecionadas visualmente 17 telas: botões J1 (`8589934761`), teclado/mouse J1 (`8589935181`), zonas mortas (`8589935764`), dispositivos J1/J2 (`8589934677`, `8589936082`), transferência (`8589939397`), gatilhos (`8589934929`), analógicos (`8589935013`), vibração J1/J4 (`8589935097`, `8589938427`), movimento/câmera (`8589939587`), restauração (`8589935851`), direcional/cliques de controle e teclado (`8589935677`, `8589939777`), gatilhos no teclado (`8589939682`), identificar (`8589939492`) e teste de vibração (`8589935938`). A árvore das telas de acesso e sem dispositivo também foi consultada.
- Geometria de referência: frames 1920×1080; janela aproximadamente x=180, y=100, 1560×880; duas colunas com divisor em x=778. Cores observadas: fundo `#0E1220`, laranja `#F2701F`, texto `#E9ECF4`, texto secundário `#A6AFC2`. Roboto Condensed em títulos/linhas; Roboto em valores e explicações.
- Categoria selecionada usa superfície azul discreta e ponto. Linha editada usa marcador/valor laranja. A seleção da categoria deve permanecer distinguível quando o foco passa à direita.
- Valores, nomes, atribuições e simulações do protótipo são demonstrativos. Exemplo: a tela de vibração J4 mantém ações ativas com dispositivo Nenhum; não reproduzir essa inconsistência funcional. A aplicação consultará capacidades reais.
- Referência comportamental lida: `../dusklight/src/dusk/ui/controller_config.cpp/.hpp`, `settings.cpp`, `res/rml/controls.rcss` e `window.rcss`. Reaproveitar conceitos de captura armada após repouso, supressão até liberação e parada da vibração; as APIs PAD do Dusklight não existem neste projeto e não serão copiadas como integração.

## Arquitetura e comportamento confirmados na investigação (antes da implementação)
- `app/src/menu/settings.cpp`: aba Controles ainda tem backend, primeiro nome SDL, opções globais MnK, acesso à lista antiga de teclas e atalhos do menu. `Keys()` usa um único `GameConfig::key_binds`; `Capture()` abre diálogo por até cinco segundos. Restauração atual de teclas não pede confirmação.
- `Settings::Capture()` retira uma tecla duplicada de outros vínculos antes de atribuí-la ao alvo, preservando a outra alternativa. Essa política existente deve ficar limitada ao banco MnK da porta editada.
- `launcher/src/config_manager.h/.cpp`: `input_backend`, `mnk_mode`, `mnk_mouse`, `mnk_sensitivity` e 24 ações de teclas são globais. `Settings::Save()` valida TOML existente, grava temporário e substitui atomicamente antes de publicar o estado; preservar esse contrato e chaves desconhecidas.
- `app/src/menu/ui.cpp/.h`, `panel.cpp/.h`, `pad_input.cpp/.h`: um contexto RmlUi, duas colunas de opções/descrição, captura de teclas e polling SDL de navegação. `PadInput` reúne entradas de todos os controles em comandos virtuais, incluindo B→Esc, LB/RB→troca de abas e X→Del. Hoje a captura não distingue essa origem.
- `app/ui/*`, `menu/assets.cpp` e `menu/text.cpp`: tema v4 e recursos embutidos já disponíveis, incluindo Roboto e Roboto Condensed; sete idiomas. Criar documento de controles dedicado reutilizando tema e ciclo de vida da UI.
- `sdk/include/rex/input/device.h`: `kMaxGuestUsers = 4`. `InputDriver` oferece estado, capacidades, vibração e keystrokes por DeviceId. `InputSystem` implementa o roteamento para os índices guest e mantém inventário privado.
- **O app usa SlotAssignment**, em `app/src/input_replay.cpp::NativeInput` e nos caminhos de record/replay. Controles físicos seguem ordinal; dispositivos sintéticos (MnK e NOP) são acrescentados à porta 0. Não usa SharedAssignment. A atribuição atual não permite escolher ou transferir dispositivos pela interface.
- `InputSystem::SetDeviceAssignment` deve ser chamado antes do guest iniciar polling. O plano instala uma política uma vez e publica alterações por snapshots; não substituir o ponteiro de política durante leituras concorrentes.
- `sdk/src/input/sdl/sdl_input_driver.cpp`: o inventário público contém nome/GUID e identificador transitório; GUID identifica modelo, não uma unidade física. O driver possui SDL_Gamepad e pode obter serial/path e capacidades, mas ainda não os expõe. GetDeviceState neutraliza o estado quando inativo, logo essa leitura não serve diretamente para a visualização enquanto o menu bloqueia o guest.
- Cabeçalhos SDL locais confirmam `SDL_GetGamepadSerial`, `SDL_GetGamepadPath`, `SDL_GamepadHasAxis`, propriedades de rumble e `SDL_RumbleGamepad`. SDL deve ser acessado no thread de UI conforme o ciclo existente.
- `sdk/src/input/xinput/xinput_input_driver.cpp`: enumera até quatro slots, apresenta nome genérico XInput Controller e não fornece serial/modelo. Preservar o backend selecionado e apresentar identidade por slot; não misturar silenciosamente sua lista com a do SDL.
- `sdk/src/input/mnk/mnk_input_driver.cpp`: um dispositivo teclado/mouse; perfil, sensibilidade e habilitação globais. Sensibilidade válida 0.01–10.0. Capacidades sintéticas anunciam motores, embora não haja rumble físico; a UI deve distinguir MnK de um controle real.
- `app/src/mouse_camera_script.cpp`: câmera direta consome delta global do SDK e sensibilidade global; deve respeitar atribuição da porta, remapeamentos e remoção das direções do mouse. Outras portas não podem movimentar a câmera única do jogador 1 por esse atalho.
- `menu/input_gate.cpp`: neutraliza o guest e drena keystrokes; aguarda liberação antes de restaurar entrada. Preservar também nos caminhos XInput e replay.
- `sdk/src/input/nop/nop_input_driver.cpp`: NOP se anuncia como controle conectado. A opção Nenhum precisa excluí-lo da atribuição efetiva de uma porta explicitamente vazia.
- Não foi identificada lacuna de renderer/binário que exija REA/IDA: fontes locais respondem aos contratos de entrada investigados.

## Git, build e testes observados
- Branch `feat/no-launcher`; HEAD `5ab50f7ecb8049a624169291018497d6bbf5efef`.
- SDK HEAD `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`, com mudanças locais prévias. Entre elas, os arquivos do driver MnK que serão relevantes à implementação. Inspecionar e integrar sem reverter/substituir os diffs existentes.
- Sujeira inicial no superprojeto: `.gitignore`, `docs/renderers.md`, `docs/translation-mods.md`, submódulo `sdk`; não rastreados `.github/`, `app/src/gpu/diagnostics/step_split.cpp/.h`, `patches/app/`, `tools/localization/extract-subtitle-corpus.py`. Outros worktrees existem; não foram alterados.
- Não havia diretório `tasks/`; foi reservado TASK-001.
- Build existente em `app/out/build/win-amd64-release`, Ninja/Clang Release, renderer nativo D3D12 e Vulkan habilitados. Python 3.14, CMake, CTest e Clang encontrados.
- Executados quatro binários existentes, todos exit 0: `narutorise_menu_input_test` (gate/latch/keystrokes/atalho), `narutorise_setup_gate_test` (173 verificações), `narutorise_key_bindings_test` (112 teclas) e `narutorise_config_manager_test`.
- Saídas isoladas em `C:/Users/Jorge/AppData/Local/Temp/narutorise-controls-baseline-90c2d65dbbf6415f9f8baa419b6ac3af`. São evidências dos binários já disponíveis, não de uma recompilação do código atual ou do redesenho.
- `cmake --build app/out/build/win-amd64-release --target narutorise_menu_input_test -- -n` retornou 0, indicando regeneração pendente do CMake; não demonstra um build atualizado. Nenhuma implementação foi compilada nesta fase.

## Dependências, incompatibilidades e restrições
- Quatro portas são suportadas; novos modos multiplayer estão fora de escopo.
- Um dispositivo exclusivo por porta substitui a combinação antiga teclado+controle em J1. Migração DEC-002 aprovada junto do plano e implementada.
- SDL sem serial/path inequívoco e XInput por slot não permitem prometer identidade física estável de controles idênticos entre reinícios. Tratar ambiguidade explicitamente, sem tomar um dispositivo de outra porta.
- Desconexão deve invalidar captura/transferência/teste do handle antigo, preservando configurações; reconexão só retoma atribuição quando identidade for segura.
- Necessário ampliar o contrato de telemetria/identidade e permitir perfil MnK efetivo ao vivo; apenas desenhar quatro abas seria insuficiente.
- O jogo pode reagir à desconexão de J1 quando Nenhum for selecionado; verificar em boot real sem mascarar a escolha com NOP.
- Inventário físico executado: 0 gamepads SDL/XInput; teclado/mouse disponíveis. Game root extraído válido usado nos boots isolados. Não confundir SDL virtual dos testes com comprovação física.

## Implementação atual
- Novo modelo/serviço/runtime/captura em app/src/controls e janela dedicada em app/src/menu/controls.*, com assets RmlUi/RCSS e sete idiomas.
- SDK compartilha identidade real, incluindo serial+caminho quando ambos existem; expõe perfil MnK imutável. Serviço UI lê estado antes do gate e usa a mesma base hid_mappings_file do runtime.
- ConfigManager salva esquema por quatro portas com transações e preservação de chaves/alternativas; ajustes e mapas são publicados somente após save.
- InputSystem roteia por identidade, transforma estado/capacidades/keystrokes/rumble e não mapeia NOP a portas explicitamente vazias. Captura usa fonte bruta, liberação antes/depois e mantém a lista. Replay preserva portas por anotações P opcionais em NRINPUT 2, sem quebrar arquivos antigos.
- Encerramento nativo bloqueia novos comandos de motor e zera antes/depois da parada do guest, pois o SDK pode hard-exit sem OnShutdown. Caminho de renderer legado mantém perfil SDK, sem forçar perfis nativos.
- Dez executáveis menores, UI nas duas APIs/sete idiomas e boots/overlay/Alt+F4 reais executados. Ver comandos e resultados em tests.md.

## Limitações de validação restantes
- Dois gamepads físicos iguais, drift, motor/firmware e reconexão físicos não foram exercitados (hardware ausente).
- A câmera na exploração foi integrada aos hooks existentes, compilada nas quatro DLLs e conferida por SDK/inspeção; avaliação visual manual durante exploração não foi realizada.

## Retomada por defeito de navegação — STEP-010
- Usuário relatou não alcançar conteúdo da categoria sem mouse e analógico navegando enquanto testava entrada. Pediu ignorar somente a frase inacabada “o ideal seria”; a correção dos dois problemas continua solicitada e dentro da aprovação original.
- Corrigido foco inicial no conteúdo em ordem de documento, navegação espacial dos botões/alternativas e B/Esc para voltar à categoria. Sticks não geram comandos de navegação na página de zonas mortas; direcional permanece ativo e neutral é exigido ao sair.
- Um DualSense SDL foi detectado nesta retomada, sem validação manual de botões/motores. As fixtures UI são isoladas do hardware por hints SDL no executável de teste. Ver resultados novos em tests.md, TEST-022/023; estado DONE.
