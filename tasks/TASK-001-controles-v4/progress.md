# Progresso — TASK-001

Estado da task: DONE — correção de navegação reportada pelo usuário implementada e validada em STEP-010.
Aprovação de implementação: APPROVED — plano original; STEP-010 corrige REQ-002/012 já aprovados, sob solicitação de correção desta sessão. Não se adota como requisito a frase inacabada “o ideal seria...”, que o usuário pediu para ignorar.
Origem e data da aprovação: usuário respondeu “aprovado” nesta sessão em 2026-10-25 após apresentação do plano completo. Steering adicional: trabalhar na mesma branch do no-launcher (`feat/no-launcher`), sem novo worktree.

| Etapa | Estado | Evidência | Próxima ação |
|---|---|---|---|
| STEP-001 | DONE | Build Clang e controls_config_test exit 0; migração/round-trip/limites/transação/isolamento | Integração do inventário em STEP-002 |
| STEP-002 | DONE | SDK runtime recompilado/instalado; controls_service_test PASS; SDL=0 e XInput=0 controles físicos | STEP-003 |
| STEP-003 | DONE | SDK atualizado; controls_runtime_test e menu_input_test compilados/executados exit 0 | STEP-004 |
| STEP-004 | DONE | native_engine_menu_test: 179 registros, 0 falhas, verdict true; screenshot real inspecionado | STEP-005 |
| STEP-005 | DONE | controls_capture_test PASS; UI integrada 358 registros, 0 falhas; SDL virtual B/LB/RB/dpad/cliques/6 eixos | STEP-006 |
| STEP-006 | DONE | UI integrada: 417 registros, 0 falhas; transferência/cancelamento J1–J4, Nenhum e hotplug SDL virtual | STEP-007 |
| STEP-007 | DONE | UI: 482 registros/0 falhas; preview real SDL, toggle, rumble/erro/parada/prazo/foco/saída | STEP-008 |
| STEP-008 | DONE | UI: 508 registros/0 falhas; boot real D3D12/Vulkan, 4 processos exit 0, persistência/reset isolados | STEP-009 |
| STEP-010 | DONE | Reprodução inicial falhou; build atual, UI D3D12/Vulkan 775 registros cada/zero falhas e quatro boots de persistência PASS | Validação manual com controle físico |
| STEP-009 | DONE | Build completo; 10 testes menores PASS; UI D3D12/Vulkan 639 registros cada/0 falhas; boots/overlay/10 Alt+F4 PASS; patches/traceabilidade PASS | Validação física/manual quando houver hardware/contexto |

## Registro
- 2026-10-25: lidos AGENTS.md, TASK_PROTOCOL.md, skills task-discover/task-plan, templates e código atual. Não havia task anterior.
- 2026-10-25: aberto documento indicado pelo MCP FigCraft, confirmado fluxo/página e inspecionadas visualmente 17 telas. Lidas referências Dusklight.
- 2026-10-25: conferidos branch, HEAD, diffs locais e outros worktrees; nenhum diff preexistente alterado.
- 2026-10-25: quatro binários básicos existentes executados com exit 0. Dry-run de build indica CMake pendente de regeneração; nenhuma recompilação efetuada. Evidências e ressalvas em context/tests.
- 2026-10-25: criados sete documentos de planejamento; nenhum arquivo de implementação alterado.
- 2026-10-25: `python scripts/validate_tasks.py` passou com 1 task e 0 problemas. Revisão adicional confirmou sete documentos, nove etapas, vinte casos, todos os 19 requisitos cobertos por testes e ausência de whitespace final.

## Bloqueios
- Nenhum bloqueio de aprovação. Plano aprovado nesta sessão.
- Inventário original: 0 gamepads; na retomada há um DualSense SDL detectado, sem acionamento automático de seus botões ou motores. TEST-004/005/015/016/020 conservam lacunas físicas/manuais; TEST-008 tem avaliação visual manual da câmera em exploração BLOCKED. Implementação e validações automatizáveis concluídas.

- 2026-10-25 STEP-001: executado out/no-launcher/build.ps1 -Configure -Target narutorise_controls_config_test (exit 0) e binário novo com diretório out/no-launcher/controls-v4/config-tests (exit 0). Código salvo na branch feat/no-launcher.

- 2026-10-25 STEP-002: service_test novo passou; identidades repetidas/ambíguas recusadas, snapshots e shutdown verificados. SDK rexruntime Release recompilado e instalado com ponte de identidade. Rumble físico/hotplug permanecem sem hardware (0 gamepads em SDL/XInput).

- 2026-10-25 STEP-003: controls_runtime_test PASS (InputSystem verdadeiro com drivers de teste: transferências, Nenhum desconectado, teclas remapeadas, zonas mortas, rumble escalado, perfil MnK J2, migração); menu_input_test recompilado PASS. Hook de câmera usa sensibilidade exportada do perfil SDK.

- 2026-10-25 STEP-004: janela dedicada renderizada e inspecionada; ajuste de categoria/marcador à direita, foco azul, rodapé com setas vetoriais e viewport sem corte. Teste integrado D3D12 passou com quatro abas, bancos MnK, percursos e atalhos antigos preservados. Screenshots em step-004-ui*.bgra; comparação completa em resoluções/APIs adicionais fica em STEP-009.

- 2026-10-25 STEP-005: novo capture_test PASS; native_engine_menu_test 358 registros/0 falhas/verdict true. Captura in-line, Esc/Del, teclado/mouse/conflitos locais, PageDown como entrada sem trocar jogador, SDL virtual B/LB/RB/dpad/LS/RS e seis eixos, bloqueio inicial/final exercitados. Screenshot da linha em captura inspecionado. SDK inclui X1MB/X2MB reais.

- 2026-10-25 STEP-006: native_engine_menu_test passou, 417 registros e zero falhas; teclado transferido por J1/J2/J3/J4, cancelamento sem gravação, ajustes preservados, Nenhum efetivo e reassignment sem confirmação para disponível. SDL virtual: desconexão cancela captura, reconexão não reutiliza handle, transferência pendente invalidada sem gravação.

- 2026-10-25 STEP-007: preview de dois analógicos e dois gatilhos, região radial ignorada e saída efetiva inspecionados visualmente. SDK SDL anuncia rumble físico real; UI navega pelo backend ativo, com backoff de slots XInput ausentes. Teste integrado PASS (482 registros): leitura SDL virtual, toggle, ajuste isolado, Identify 250 ms sem transferir, intensidades, parar, sair/foco/fechar, timeout 5 s e falha do backend. Hardware físico continua não disponível.

- 2026-10-25 STEP-008: restauração J1/J3 confirmou ambos bancos+ajustes, dispositivo mantido e demais portas intactas; cancelamento sem save. Config test cobre erro de escrita, load sem alteração parcial e esquema futuro preservado. Boot real copiado em Temp: D3D12/Vulkan, quatro processos exit 0, persistência/reabertura/reinício/Esc/Del/reset cancelado/confirmado e screenshots 720p/1080p/21:9. Log step-008-boot-4.log/results.json. Tentativas anteriores falharam na automação de Del por falta do bit de tecla estendida; o teste foi corrigido e reexecutado.

- 2026-10-25 STEP-009: build completo e dez executáveis menores PASS; UI D3D12/Vulkan PASS (639 registros cada), sete idiomas, câmera/mouse na linha, limites, save recusado, SDL virtual de mesmo modelo, eixos assinados e ciclo de vida. Boot real, overlay e dez Alt+F4 passaram; patches reproduzem SDK e reconhecem reaplicação. Restam somente comprovações físicas/manuais declaradas em tests.md. Fechamento documental e checagem final concluídos.

- 2026-10-25 conclusão: último build `final-delivery-build.log` exit 0; `final-qa-d3d12.jsonl` e `final-qa-vulkan.jsonl` PASS, 639 registros cada e zero falhas; nomes de câmera legíveis e captura/foco conferidos visualmente. Nova execução `final-persistence` passou em quatro processos. Configuração anterior do produto de build comparada ao backup e preservada (não precisou restauração). Branch feat/no-launcher confirmada. Nenhuma aprovação pendente ou etapa de implementação restante.

- Retomada após relato do usuário: preservar feat/no-launcher e diffs existentes; correção limitada ao percurso de foco e ao uso dos analógicos no teste. Aprovação original persiste; não houve nova ampliação de escopo.
- 2026-10-25 STEP-010: teste novo reproduziu a falta de acesso aos campos (focus-repro.jsonl, 647 registros, categoria não entra no conteúdo). Seleção agora foca o primeiro botão habilitado em ordem de documento; B/Esc retorna à categoria e setas/direcional alcançam alternativas/ajustes. Preview usa eixos somente nos gráficos e exige neutral antes de reativar navegação por stick.
- Build focus-product-build.log exit 0; focus-final-d3d12.jsonl e focus-final-vulkan.jsonl: 775 registros cada, zero falhas/verdict true. TEST-022/023 PASS; captura in-line/B/LB/RB/Esc/Del/transferências/hotplug continuam passando. focus-persistence: quatro processos exit 0, autosave/reabrir/reiniciar/cancelar/remover/restaurar/isolamento e screenshots PASS. Configuração atual do build preservada byte a byte (backup focus-user-config-before-build.toml).
- Aplicação recompilada reaberta com --setup para revisão do usuário (PID 23404 e janela visível confirmados). Inventário registrado em focus-service.log: 1 DualSense SDL com rumble, 0 XInput; somente enumeração e rumble inválido no service_test, exit 0. Sem comando de vibração para o controle físico.
