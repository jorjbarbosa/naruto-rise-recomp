# Handoff — TASK-003

- Estado: DONE. Aprovação explícita do usuário em 2026-10-25: “pode implementar a tela e a troca com o asset”, após F0 v7 centralizado e ajuste DLC.
- Branch/commit-base: feat/no-launcher / 4355dd2364cf0322113b25a158dac134841e25f8. Não criado commit; alterações anteriores preservadas.
- Etapas: STEP-001 investigação, STEP-002 leitor e STEP-003 UI/integração concluídas.
- Código novo: app/src/menu/game_logo.h/.cpp; app/tests/game_logo_test.cpp e prelaunch_logo_boot_test.py.
- Integração: setup_screen.*, render_interface.*, ui.cpp, narutorise_app.h e CMakeLists.txt. Layout/vocabulário: setup.rml/.rcss, text.cpp. assets.cpp deixa de embutir capa/fade/espiral/kunai. Testes GPU e ROI da regressão de setup atualizados para o menu centralizado.
- Comportamento: sem jogo, título textual e Escolher o jogo; com jogo válido, Jogar/Configurações/DLC/Sair e logo local 528×288. SRA ausente/corrompido mantém texto. Worker lê/decomprime fora da UI; somente origem ainda ativa publica textura na thread UI; resultados obsoletos descartados.
- Validações: reader-final com fixtures, pasta real e ISO real; pixels idênticos à investigação. 859 checks GPU/UI em D3D12 e 859 em Vulkan. 8 casos de boot do logo; 15 regressões de setup, incluindo sete idiomas e handover. game_image 20 checks; setup_gate 205 checks; game_choice, localization e py_compile passaram.
- Evidências: out/prelaunch-implementation-2026-10-25/ (ignorado). Capturas finais: boot-final/d3d12-select-iso-before.png e boot-final/vulkan-select-iso.png. Report do leitor: reader-final/pixel-verification.json.
- Executável: app/out/build/win-amd64-release/narutorise.exe, compilado Release. A tela pode ser aberta com --setup; não há asset proprietário novo embutido.
- Limites: verificado nesta revisão local do jogo. Outras revisões/arquivos inválidos recebem fallback; isso não é parecer de direitos autorais. Artefatos extraídos/capturas ficam locais; nenhum release publicado.
- Próximo passo: revisão visual do usuário ou publicação em fluxo separado, se solicitada. Não há implementação ou teste obrigatório pendente desta task.



## Integração posterior — 2026-10-25

O usuário autorizou a faxina e os commits da branch. O código pendente desta entrega foi integrado em `44462fd`. Os resultados acima são os registros das execuções da task; a regressão de interface da integração posterior teve uma violação de acesso ainda sem causa determinada. Consultar o [checkpoint atual](../../docs/handoffs/live-settings-2026-10-25.md) e o [registro de QA](../../docs/qa/menu-access-violation-2026-10-25.json) antes de declarar a integração completa validada. A investigação foi adiada pelo usuário para outra sessão.
