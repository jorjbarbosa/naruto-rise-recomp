# Testes — TASK-001

## Evidência anterior à implementação (2026-10-25)
Foram executados os binários já presentes em `app/out/build/win-amd64-release`, sem recompilação: menu_input exit 0 (gate, packet, keystrokes drenados, latch de liberação e atalho), setup_gate exit 0 (173 verificações), key_bindings exit 0 (112 teclas) e config_manager exit 0 (persistência/configuração antiga). Dados temporários: `C:/Users/Jorge/AppData/Local/Temp/narutorise-controls-baseline-90c2d65dbbf6415f9f8baa419b6ac3af`.

O comando `cmake --build app/out/build/win-amd64-release --target narutorise_menu_input_test -- -n` terminou em 0 e indicou regeneração pendente. Nenhum novo build foi executado. Esses resultados não cobrem o redesenho nem demonstram que os binários refletem todos os diffs atuais. Os estados atuais dos casos abaixo incorporam as execuções registradas; as limitações físicas/manuais estão explícitas. Fixtures/dispositivos simulados são exclusivos de testes; não entram na UI de produção.

### TEST-001 — Esquema, migração e round-trip por quatro portas
- Requisitos: REQ-003, REQ-009, REQ-010, REQ-015, REQ-019.
- Etapas: STEP-001, STEP-009.
- Tipo: unitário/integração de configuração.
- Procedimento/comando planejado: novo controls_config_test em diretório temporário; arquivos sem bloco controls, arquivo legado com mnk_mode on/off, dois slots e modificadores, quatro perfis distintos, fontes sem vínculo, duplicadas e metadados de identidade. Salvar/carregar e comparar todos os bancos; preservar chaves alheias. Resolução inicial usa inventário de teste explícito, sem confundir fixture com hardware.
- Resultado: PASS
- Evidência: controls_config_test: migração, alternativas inclusive 40 posições, aliases, quatro perfis e round-trip; controls_runtime_test: prioridade de dispositivo da migração.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-002 — Limites e transações em persistência
- Requisitos: REQ-011, REQ-013, REQ-014, REQ-015, REQ-016, REQ-019.
- Etapas: STEP-001, STEP-008, STEP-009.
- Tipo: unitário/integração de configuração.
- Procedimento/comando planejado: valores fora de faixa, NaN quando aplicável, extremos de sensibilidade/eixos/gatilhos/motores e deadzone; TOML malformado, destino não gravável e falha de substituição. Verificar que não publica estado novo nem apaga arquivo/chaves anteriores. Transferência e reset gravam uma única transação.
- Resultado: PASS
- Evidência: Config test: sensibilidade 0.01/10, limites inválidos/NaN/infinito, deadzones 0/0.95/1, motores 0/1/fora de faixa, fontes inválidas, falha real de escrita, arquivo malformado e esquema futuro; UI integrada recusou save sem substituir TOML nem publicar estado.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-003 — Cancelamentos sem gravação e isolamento dos bancos
- Requisitos: REQ-003, REQ-009, REQ-010, REQ-015, REQ-016.
- Etapas: STEP-001, STEP-008, STEP-009.
- Tipo: unitário/integração.
- Procedimento/comando planejado: comparar bytes/estado antes e depois de cancelar captura/transferência/reset; editar pad e MnK em portas diferentes; preservar alternativas e remover duplicata MnK somente na porta/banco alvo conforme política existente.
- Resultado: PASS
- Evidência: TOML comparado em cancelamentos, isolamento dos bancos/portas e alternativas preservadas em config_test e UI; boot real confirma cancelamento de captura/restauração sem gravação.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-004 — Inventário e identidade de unidades iguais
- Requisitos: REQ-004, REQ-006, REQ-018, REQ-019.
- Etapas: STEP-002, STEP-006, STEP-009.
- Tipo: integração de backend e manual com hardware.
- Procedimento/comando planejado: comparar inventário com SDL/XInput ativo; dois controles do mesmo modelo com e sem serial; teclado/mouse e Nenhum; nenhuma entrada NOP/replay. Validar sufixos, status, identidade de sessão e restauração segura; XInput usa slot real e declara limitação. Verificação física requer unidades disponíveis.
- Resultado: BLOCKED
- Evidência: Automatizado PASS: inventário real SDL=0/XInput=0, teclado real; dois SDL virtuais de mesmo nome com identidades/sufixos distintos e rejeição de identidade ambígua. Parte com dois controles físicos não executada: hardware indisponível.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-005 — Hotplug, telemetria bruta e lifetime
- Requisitos: REQ-004, REQ-006, REQ-012, REQ-017, REQ-018.
- Etapas: STEP-002, STEP-006, STEP-009.
- Tipo: integração/manual.
- Procedimento/comando planejado: conectar/desconectar com lista, captura, confirmação e teste abertos; repetir reconexão enquanto outro controle permanece conectado. Guest bloqueado continua neutro, telemetria física continua atualizada, handle removido não passa a outra unidade. Confirmar ausência de chamadas SDL fora do thread apropriado.
- Resultado: BLOCKED
- Evidência: Automatizado PASS: SDL virtual desconectado durante captura, teste de rumble e confirmação; reconexão não assume handle antigo; telemetria independe do guest e roteamento acompanha metadata. Conexão/desconexão física não executada: 0 gamepads.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-006 — Atribuição real e todas as operações guest
- Requisitos: REQ-003, REQ-009, REQ-010, REQ-017, REQ-018.
- Etapas: STEP-003, STEP-009.
- Tipo: integração com drivers controlados.
- Procedimento/comando planejado: alimentar InputSystem real com drivers de teste, quatro usuários e perfis distintos; conferir estado/capacidades/keystrokes/rumble após troca e transferência, inclusive user_index dos eventos, packet após remap e descarte de eventos antigos. Nenhum retorna desconectado e não recebe fallback NOP.
- Resultado: PASS
- Evidência: controls_runtime_test executa InputSystem real com drivers controlados: estado/capacidades/keystrokes/rumble, transferência, eventos sem histórico obsoleto, novo owner e refresh de identidade. Portas vazias desconectadas; TEST-021 cobre gravação/replay.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-007 — Transformação de fontes e zonas mortas
- Requisitos: REQ-009, REQ-011, REQ-012, REQ-017, REQ-018.
- Etapas: STEP-003, STEP-007, STEP-009.
- Tipo: unitário/integração.
- Procedimento/comando planejado: pad conhecido com botões/dpad/LS/RS/LT/RT, sinais invertidos e vínculos removidos; testar repouso, abaixo/no/acima do limiar, diagonais, saturação e enable off. LS/RS e LT/RT independentes; valor exibido pelo preview efetivo coincide com estado entregue ao guest.
- Resultado: PASS
- Evidência: Runtime test: repouso, limiar, saída reescalada, desativação e eixo removido; UI SDL virtual: dez amostras de eixos (quatro analógicos nos dois sentidos e dois gatilhos), preview/filtro/toggle e ajustes independentes.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-008 — MnK e câmera efetivos sem vazamento de porta
- Requisitos: REQ-003, REQ-009, REQ-010, REQ-013, REQ-017, REQ-019.
- Etapas: STEP-003, STEP-007, STEP-009.
- Tipo: integração/boot.
- Procedimento/comando planejado: teclado em J1 e J2 alternadamente, perfis diferentes e controle em outra porta; teclas simples/modificadores, botões e direções de mouse, movimento/câmera e sensibilidade extrema válida. Remover/inverter direções e verificar câmera direta/fallback right stick sem delta duplicado. Teclado em J2 não move diretamente câmera de J1.
- Resultado: BLOCKED
- Evidência: Automatizado/inspeção PASS: perfil MnK da porta proprietária, tecla/release, mouse no right stick em J2 com hook ativo, remoção/remapeamento de direção para movimento sem vazamento, sensibilidade, UI captura movimento na própria linha. Quatro DLLs da câmera compiladas. Avaliação visual manual da câmera durante exploração não realizada; não é reivindicada prova em gameplay.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-009 — Estrutura e comparação visual v4
- Requisitos: REQ-001, REQ-002, REQ-003, REQ-018, REQ-019.
- Etapas: STEP-004, STEP-009.
- Tipo: teste RmlUi/visual/manual.
- Procedimento/comando planejado: estender native_engine_menu_test/menu_visual_test.py para capturar janela dedicada, abas, foco vs categoria, captura na linha, lista/transferência, bancos de teclado, zonas mortas, rumble e restauração; comparar com MCP FigCraft atual, sem alterar documento. D3D12/Vulkan em 1280×720, 1920×1080 e 21:9; verificar textos nos sete idiomas.
- Resultado: PASS
- Evidência: UI integrada D3D12/Vulkan, sete idiomas renderizados, capturas inspecionadas; boot real captura 1280x720, 1920x1080 e 2560x1080 nas duas APIs. Comparado ao documento atual MCP FigCraft v4 (mantido sem alterações). Gráficos/captura/transferência/reset examinados.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-010 — Percursos de categorias e bancos próprios
- Requisitos: REQ-002, REQ-003, REQ-010, REQ-018.
- Etapas: STEP-004, STEP-009.
- Tipo: integração UI.
- Procedimento/comando planejado: abrir pela aba Controles; navegar botões↔direcional/cliques, gatilhos, analógicos ou movimento/câmera, zonas mortas e vibração; trocar J1–J4 via clique/LB/RB fora de captura. Repetir com teclado e Nenhum, garantindo retorno ao banco/categoria apropriados e recursos indisponíveis explicados.
- Resultado: PASS
- Evidência: UI integrada e boot real: acesso dedicado, botões/direcional/cliques/gatilhos/movimento-câmera, retorno ao banco MnK, quatro abas, troca fora da captura, categoria preservada quando compatível e recursos indisponíveis desabilitados.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-011 — Captura na linha, Esc e Del
- Requisitos: REQ-007, REQ-008, REQ-009, REQ-010, REQ-015.
- Etapas: STEP-005, STEP-009.
- Tipo: unitário da máquina de estados e integração UI.
- Procedimento/comando planejado: iniciar por Enter, A e clique; observar mesma lista/scroll, valor Pressione uma entrada… e rodapé. Aplicar nova fonte, Esc físico preserva valor/TOML, Del físico limpa só alvo/encerra. Não existe modal/página de remapeamento. Restrições/conflitos são apresentados sem trocar de tela.
- Resultado: PASS
- Evidência: capture_test e UI: captura na mesma lista e valor, início por Enter/A/clique, rodapé, Esc sem gravação e Del limpa. Boot real Windows reabriu/reiniciou/cancelou/removeu; menu key/teclas reservadas respeitadas.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-012 — B/LB/RB, bloqueio e liberação antes/depois
- Requisitos: REQ-007, REQ-008, REQ-009, REQ-015.
- Etapas: STEP-005, STEP-009.
- Tipo: integração UI com sequências brutas e hardware quando disponível.
- Procedimento/comando planejado: manter iniciador pressionado antes de armar, soltar e capturar B/LB/RB individualmente; nenhum volta/troca jogador. Durante captura clicar abas/categoria/ajustes/fechar, mover direcional e usar atalhos de menu: estado de navegação permanece fixo. Manter capturado pressionado após término, repetir polling e soltar; nenhuma ação atrasada. Repetir cancelando e removendo.
- Resultado: PASS
- Evidência: SDL virtual no caminho de produção: A iniciador aguardou liberação; B/LB/RB/dpad/cliques capturados sem voltar/trocar jogador, mantidos pressionados sem ação tardia. PageDown físico é capturado no teclado sem navegar. Supressão inicial/final e navegação bloqueada verificadas.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-013 — Matriz de fontes capturáveis
- Requisitos: REQ-008, REQ-009, REQ-010, REQ-018.
- Etapas: STEP-005, STEP-009.
- Tipo: integração/manual.
- Procedimento/comando planejado: A/B/X/Y/LB/RB/Back/Start, quatro direções dpad, LS/RS, LT/RT e cada direção dos dois analógicos; teclado, modificadores suportados, botões/movimento de mouse suportados. Capturar e conferir entrada guest correspondente, não apenas rótulo UI. Repetir percursos MnK sem migrar ao banco pad; entrada não suportada não cria valor sem efeito.
- Resultado: PASS
- Evidência: UI/SDL virtual: botões, dpad, LS/RS, gatilhos e eixos nos dois sentidos; teclado, botão de mouse e movimento MouseLeft capturado na câmera, conflito local e foco mantido no campo. Backend SDK: mouse removido/remapeado efetivo; capacidades rejeitam fonte sem suporte.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-014 — Atribuição e transferência entre as quatro portas
- Requisitos: REQ-003, REQ-004, REQ-005, REQ-006, REQ-015.
- Etapas: STEP-006, STEP-009.
- Tipo: integração UI/runtime e manual.
- Procedimento/comando planejado: selecionar disponível/ocupado/Nenhum, confirmar e cancelar origem/destino em J1–J4; comparar configurações completas antes/depois; transferir também teclado. Desconectar ou mudar proprietário antes de confirmar deve invalidar operação obsoleta. Reabrir lista e conferir estado real e TOML; ajustes anteriores permanecem na porta anterior vazia.
- Resultado: PASS
- Evidência: UI e TOML real: teclado transferido por J1/J2/J3/J4, confirmação/cancelamento byte a byte, origem vazia e ajustes dos dois perfis preservados. SDL virtual: dispositivo disponível/ocupado e confirmação invalidada ao remover.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-015 — Preview real e indisponibilidade por capacidades
- Requisitos: REQ-006, REQ-011, REQ-012, REQ-013, REQ-018.
- Etapas: STEP-007, STEP-009.
- Tipo: integração UI e manual com hardware.
- Procedimento/comando planejado: observar LS/RS/LT/RT reais em repouso/drift/movimento; mudar enable e valor; região/estado ignorado respondem aos dados reais, sem simulações. Teclado/Nenhum/eixo ausente exibem motivo e não gráfico inventado; desconexão interrompe amostras. Diferenciar visualização bruta de filtro já imposto pelo hardware/driver.
- Resultado: BLOCKED
- Evidência: Automatizado PASS: preview usa SDL real de dispositivo virtual, raw mapeado/região/saída, drift/movimento/toggle e bounds; teclado/Nenhum sem gráfico inventado. Leitura e drift de dispositivo físico não executados por falta de gamepad.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-016 — Intensidades, identificar e parada garantida de rumble
- Requisitos: REQ-006, REQ-014, REQ-015, REQ-018.
- Etapas: STEP-007, STEP-009.
- Tipo: integração com backend observável e teste físico.
- Procedimento/comando planejado: verificar valores dos dois motores por porta e no rumble guest, identificar unidade ocupada sem transferir, iniciar/parar teste, expiração e falha de backend. Sair/trocar aba/categoria/dispositivo, perder foco, fechar aplicação e desconectar enviam parada/invalidação correta; teclado/Nenhum/sem suporte não oferecem teste ativo. Físico exige controle com motores funcionais; não considerar chamada simulada como vibração comprovada.
- Resultado: BLOCKED
- Evidência: Automatizado PASS: callback SDL observa intensidades, Identify sem transferir, Parar, prazo 5 s, mudança de jogador/categoria, foco, fechar, erro e cancelamento/invalidação na desconexão. Backend removido não aceita novo motor write; não se interpreta a última chamada histórica como motor físico ativo. Parada física/firmware não comprovada: 0 gamepads. Encerramento nativo também zera antes/depois de parar guest, por inspeção do caminho hard-exit.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-017 — Restaurar os dois bancos da porta mantendo dispositivo
- Requisitos: REQ-003, REQ-010, REQ-014, REQ-015, REQ-016.
- Etapas: STEP-008, STEP-009.
- Tipo: integração UI/config/runtime.
- Procedimento/comando planejado: alterar pad/MnK e ajustes em todas as portas, abrir confirmação e conferir alcance; cancelar não altera bytes; confirmar restaura os dois bancos/LS/RS/LT/RT/sensibilidade/rumble apenas da porta escolhida e preserva dispositivo/atalhos/outras portas. UI/runtime/TOML mostram os mesmos padrões.
- Resultado: PASS
- Evidência: UI restaura J1 e J3: dois bancos/ajustes completos retornam aos padrões, mantém dispositivo e demais portas; Cancel default não grava. Boot real restaura e preserva J2/J3/J4, chave alheia e teclado atribuído.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-018 — Reabrir interface e reiniciar processo com persistência
- Requisitos: REQ-003, REQ-004, REQ-010, REQ-015, REQ-016, REQ-019.
- Etapas: STEP-008, STEP-009.
- Tipo: boot isolado, novo controls_boot_test.py.
- Procedimento/comando planejado: produto/configuração/dados em Temp; atribuir, remapear, remover, ajustar, transferir e restaurar em portas diferentes; fechar/reabrir janela e terminar/iniciar nova aplicação. Comparar UI, TOML e entrada efetiva. Repetir com dispositivo desconectado e identidade ambígua, sem seleção silenciosa incorreta; não modificar instalação/ISO/config pessoal.
- Resultado: PASS
- Evidência: controls_boot_test.py: quatro processos reais (primeiro/reiniciado por D3D12/Vulkan), autosave/reabertura/reinício, Esc/Del e reset com configuração em Temp. Screenshots do processo reiniciado exibem vínculo salvo.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-019 — Regressões, build e patches reaplicáveis
- Requisitos: REQ-002, REQ-008, REQ-013, REQ-017, REQ-019.
- Etapas: STEP-009.
- Tipo: build/testes de regressão.
- Procedimento/comando planejado: compilar targets afetados no ambiente VS/Clang atual, executar novos testes e menu_input/setup_gate/key_bindings/config_manager atualizados, native_engine_menu_test e boots aplicáveis ao fluxo antigo. Verificar caminhos record/replay e menu aberto/fechado com botões segurados. Conferir patch SDK contra base apropriada sem modificar worktree de terceiros nem descartar mudanças existentes. Registrar comandos/exit/binário/data.
- Resultado: PASS
- Evidência: Build completo atual, 10 executáveis menores PASS, UI D3D12/Vulkan PASS, overlay/game input e Alt+F4 (10 cenários) PASS. Patches SDK aplicados em cópia da revisão fixada e comparados; todos os 10 patches reconhecidos como já aplicados. Diffs locais alheios preservados.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

### TEST-020 — Aceitação física e registro de limitações
- Requisitos: REQ-001, REQ-003, REQ-004, REQ-005, REQ-006, REQ-007, REQ-008, REQ-009, REQ-010, REQ-011, REQ-012, REQ-013, REQ-014, REQ-015, REQ-016, REQ-017, REQ-018, REQ-019.
- Etapas: STEP-009.
- Tipo: manual/hardware/documentação.
- Procedimento/comando planejado: inventariar hardware real disponível e percorrer matriz do pedido em SDL e XInput quando aplicável, dois controles iguais se disponíveis, quatro portas (sem exigir quatro controles simultâneos), teclado/mouse e rumble. Registrar por caso o que foi efetivamente executado, o que foi simulado e o que ficou BLOCKED por hardware ausente. Entrega inclui arquivos, screenshots/limitações e resultados sem declarar teste físico não realizado.
- Resultado: BLOCKED
- Evidência: Inventário de hardware executado: nenhum gamepad SDL/XInput, teclado/mouse disponíveis. Aceitação automática, backend virtual e visual executados e documentados. Dois controles físicos iguais, drift/vibração/reconexão físicos e avaliação manual de câmera em exploração permanecem não comprovados.
- Data/ambiente: 2026-10-25, Windows/Clang Release, AMD Radeon RX 9070 XT; configs/dados isolados. Artefatos em out/no-launcher/controls-v4.

## Evidência STEP-002 — 2026-10-25
Build rexruntime Release + cmake --install e build narutorise_controls_service_test passaram. Execução exit 0: inventário SDL real=0 gamepads, XInput real=0 gamepads, teclado/mouse sem rumble; snapshots, shutdown e identidade ambígua verificados. TEST-004/005 têm cobertura automatizada parcial; reconexão de controles físicos e rumble aguardam hardware. Logs out/no-launcher/controls-v4/step-002-*.log.

## Evidência STEP-003 — 2026-10-25
SDK rexruntime recompilado/instalado; build e execução dos targets narutorise_controls_runtime_test e narutorise_menu_input_test passaram, exit 0. TEST-006/007: operações do InputSystem, transferência ao vivo, eventos do mapa efetivo, Nenhum, rumble escalado e transformações com toggle verificados. TEST-008: perfil MnK da porta proprietária, tecla/removal/release, sensibilidade, capacidades sem motores e câmera direta bloqueada fora de J1 verificados; movimento/câmera em boot ainda pendente. Logs step-003-*.log.

## Evidência STEP-004 — 2026-10-25
Build native_engine_menu_test e execução D3D12 passaram, 179 registros, zero ok=false e verdict passed=true; capturado e inspecionado controls-dedicated-1280x720. TEST-009/010: quatro abas, teclado/MnK próprio, direcional/retorno, movimento/câmera, vibração desabilitada e troca de jogador verificados. Fidelidade em 1080p/wide/Vulkan permanece para STEP-009. Log step-004-ui-refined.jsonl.

## Evidência STEP-005 — 2026-10-25
SDK atualizado, controls_capture_test PASS e native_engine_menu_test PASS (358 registros, nenhuma falha), log step-005-ui-fixed.jsonl. TEST-011/012/013 executados com teclado/mouse e controle SDL virtual de teste: início por A e espera da liberação, B/LB/RB sem voltar/trocar jogador, dpad/cliques, seis eixos, supressão enquanto segurado, Esc, Del, tecla reservada, PageDown capturável, preservação de outras portas/bancos, conflito LMB somente local. Screenshot capture-inline-1280x720 inspecionado. Não se trata de validação física de gamepad.

## Evidência STEP-006 — 2026-10-25
Build e execução native_engine_menu_test passaram com 417 registros, zero falhas; log step-006-ui.jsonl. TEST-014 e parte de TEST-004/005: transferência real do único teclado por quatro portas, cancelamento com comparação de bytes do TOML, dois perfis preservados, Nenhum e disponibilidade; SDL virtual desconectado durante captura e confirmação, reconexão com nova identidade de sessão e nenhuma transferência obsoleta. Screenshot transfer-1280x720 capturado. Hardware físico continua ausente.

## Evidência STEP-007 — 2026-10-25
controls_runtime_test PASS e native_engine_menu_test PASS, 482 registros/zero falhas/verdict true, log step-007-ui-final.jsonl. TEST-007/008/015/016: gráfico com leitura SDL virtual real, drift ignorado/entrada ativa/toggle/região/saída e bounds do layout; intensidades observadas no callback SDL, Identify breve, teste/parar/expiração 5 s/perda de foco/saída/fechar/erro. Captura deadzone-preview-1280x720 inspecionada após correção de display/border-radius no RmlUi. Sem prova de vibração física (0 gamepads reais).

## Evidência STEP-008 — 2026-10-25
controls_config_test PASS e native_engine_menu_test PASS (508 registros/zero falhas), incluindo restauração de J1/J3 com defaults dos dois bancos/ajustes e dispositivo mantido; cancelamento sem autosave e outras portas intactas. Produto completo compilado (incluindo quatro DLLs de câmera). Comando: python -I app/tests/controls_boot_test.py app/out/build/win-amd64-release game out/no-launcher/controls-v4/boot-acceptance-4 both. Resultado PASS: quatro processos reais D3D12/Vulkan, todos exit 0, captura/remapeamento, reabertura, reinício, Esc sem gravação, Del, restauração cancelada/confirmada, device mantido e J2/J3/J4 intactos. TEST-017/018 cobertos. Tentativas boot-acceptance/2/3 falharam pelo envio de Del sem flag estendida na automação; correção no script e execução completa posterior PASS. Screenshots em boot-acceptance-4, incluindo 720p/1080p/2560×1080.

### TEST-021 — Gravação/replay preserva portas e transferências
- Requisitos: REQ-003, REQ-017, REQ-019.
- Etapas: STEP-009.
- Tipo: integração com InputSystem e driver MnK reais, fixtures de gravação.
- Procedimento/comando planejado: narutorise_controls_replay_test <diretório temporário>; gravar teclado J3, transferir a J1 e reproduzir; testar quatro portas, None, arquivo NRINPUT 2 antigo, relógios por tempo e por leituras e lifetime após Shutdown.
- Resultado: PASS
- Evidência: execução do novo binário compilado; record/replay real da interface de teclado, fixtures legadas/portas e transferências verificadas. Anotações P opcionais conservam o cabeçalho e os eventos D/S/K antigos.
- Data/ambiente: 2026-10-25, Windows/Clang Release, arquivos isolados em out/no-launcher/controls-v4/verified-replay-tests.

## Validação consolidada de entrega — 2026-10-25

### Build e executáveis
- SDK: `cmake --build sdk/out/build/win-amd64 --config Release --target rexruntime --parallel 8`, seguido de `cmake --install sdk/out/build/win-amd64 --config Release`. Logs `verified-sdk-build.log` e `verified-sdk-install.log`.
- Produto completo e testes: `out/no-launcher/build.ps1 -Log out/no-launcher/controls-v4/final-delivery-build.log -Target narutorise,native_engine_menu_test,narutorise_controls_config_test,narutorise_controls_service_test,narutorise_controls_runtime_test,narutorise_controls_capture_test,narutorise_controls_replay_test,narutorise_controls_legacy_config_test,narutorise_controls_key_names_test,narutorise_menu_input_test,narutorise_setup_gate_test,narutorise_game_choice_test` — PASS, exit 0. Os quatro módulos da câmera também foram compilados durante a implementação.
- Dez executáveis menores PASS, todos exit 0; comandos/argumentos/saídas exatos estão em `out/no-launcher/controls-v4/final-unit-results.json`. O teste de configuração inclui extremos, arquivos inválidos, aliases e alternativas além da segunda; o de replay cobre arquivos antigos, portas/transferências e relógios por tempo/leituras.

### UI, aplicação real e visual
- `REX_NATIVE_TEST_API=d3d12` e `vulkan`, executando `native_engine_menu_test.exe out/no-launcher/controls-v4/final-qa-<api>.jsonl`: PASS nas duas APIs, 639 registros cada, nenhuma verificação `ok=false`, `verdict.passed=true`. Os registros incluem entrega/apresentação de frames e asserts; não equivalem a 639 casos de task.
- `python -I app/tests/controls_boot_test.py app/out/build/win-amd64-release game out/no-launcher/controls-v4/delivery-boot both`: PASS, quatro processos reais exit 0. Autosave, reabertura, novo processo, Esc/Del, reset/cancelamento/isolamento e capturas 720p/1080p/2560x1080. Views de dispositivos, J2 sem dispositivo, gatilhos/analógicos de teclado e direcional também capturadas. Navegar entre views não gravou mudança.
- `python -I app/tests/overlay_boot_test.py app/out/build/win-amd64-release game out/no-launcher/controls-v4/delivery-overlay`: PASS D3D12/Vulkan, dois processos exit 0; guest continuou lendo entrada com menu aberto (1584/1134 leituras no intervalo verificado), estado neutro e liberação/restauração corretos; atalhos, FPS, DLC e saída confirmada.
- `python -I app/tests/close_boot_test.py app/out/build/win-amd64-release game out/no-launcher/controls-v4/delivery-close`: PASS nos dez cenários reais de Alt+F4: jogo, overlay, settings, configuração dedicada e captura na própria linha, em D3D12/Vulkan. Todos exit 0.
- MCP FigCraft reconferiu arquivo/página/fluxo v4 atuais e `dirty=false`. Captura MCP da tela de teclado comparada à UI real. Foram inspecionadas imagens de botões, captura, transferência, restauração, gráficos e movimento/câmera; sete idiomas renderizados, incluindo português/russo. Ajustes finais corrigiram corte de nomes, região circular e hover após captura, sem simulação na UI de produção.
- Evidências visuais finais: `final-qa-d3d12.jsonl.*.bgra`, PNGs convertidos desses frames e capturas `delivery-boot/*.png`. As amostras de gráficos/gamepad vêm do SDL virtual declarado nos testes, não de motor/analógico físico.

### Reprodutibilidade e Git
- `mnk-input-driver.patch` consolidou o patch anterior sem perder suas alterações; `z-controls-v4.patch` inclui identidade/capacidades/metadata. Aplicar ambos sobre cópia da revisão SDK fixada reproduziu os nove arquivos alvo. Todos os dez patches da pilha passaram `git apply --reverse --check`; resultados em `sdk-patch-checks.json`.
- `git diff --check` passou para fontes/docs alteradas e para SDK. Patches unificados foram checados com `core.whitespace=-blank-at-eol`, pois suas linhas de contexto vazias contêm um espaço de sintaxe. A linha vazia prévia de `.gitignore` não foi alterada para mascarar o estado inicial.
- `python scripts/validate_tasks.py`: PASS (1 task, zero problemas). A task mantém 19 REQ, nove STEP e 21 TEST com IDs estáveis.
- Testes reais usaram produto/config/dados em Temp e `game/` somente como fonte. Alterações locais anteriores e outros worktrees foram preservados; branch continua `feat/no-launcher`.

### Tentativas intermediárias e limites
- A automação de Del inicialmente omitiu a flag Windows de tecla estendida; corrigida e reexecutada. Uma chamada inicial de game_choice sem o segundo argumento retornou 2; a chamada correta passou e está no JSON final.
- Os gráficos iniciais tinham containers sem display block no RmlUi; corrigidos e inspecionados. A avaliação do rumble desconectado foi corrigida para observar cancelamento/handle removido, pois o backend retirado não pode receber outro write e a última chamada do callback não representa motor físico ativo.
- Sem gamepads físicos: TEST-004/005/015/016/020 conservam BLOCKED para sua parte física. TEST-008 conserva BLOCKED para avaliação visual manual da câmera em exploração. Partes automatizadas e inspeções estão explicitadas; não se declara prova física ou gameplay manual não realizado.

## Fechamento
- `final-persistence`: nova execução do controls_boot_test, D3D12/Vulkan, primeiro/reiniciado, quatro processos exit 0 após o build final. Resultado PASS; `final-persistence/results.json` e `final-persistence.log`.
- Legibilidade da câmera corrigida e verificada por geometria real de layout: campo MouseUp 74 px, conteúdo suficiente; screenshot `final-qa-d3d12.jsonl.movement-camera-1280x720.png` inspecionada. A captura MouseLeft manteve foco e banco corretos. Interface final: 639 registros por API, zero falhas.
- Estado final dos casos: 15 PASS e 6 BLOCKED para a parte física/manual explicitamente não executada. Os bloqueios não representam código de implementação pendente.

### TEST-022 — Percurso da categoria aos campos sem mouse
- Requisitos: REQ-001, REQ-002, REQ-008, REQ-010.
- Etapas: STEP-010.
- Tipo: integração RmlUi/teclado/SDL virtual.
- Procedimento/comando planejado: a partir do foco inicial, entrar em dispositivos, voltar à categoria, selecionar Botões e Movimento/câmera, alcançar vínculos/alternativas/ajustes e voltar sem Focus direto nos campos; repetir a seleção/captura com A e provar que a confirmação inicial não captura nem edita o primeiro vínculo.
- Resultado: PASS
- Evidência: reprodução antes da correção em focus-repro.jsonl (647 registros, focus-route-category-enters-content false). Build focus-product-build.log exit 0. focus-final-d3d12.jsonl e focus-final-vulkan.jsonl têm 775 registros cada, zero falhas/verdict true: teclado e SDL virtual entram a partir do foco inicial sem Focus nos campos; alternativas, gatilhos, movimento e direcional acessíveis; confirmar categoria não começa captura; editar e voltar preservam janela/banco/porta. Os testes anteriores só injetavam foco diretamente.
- Comandos executados: `./out/no-launcher/build.ps1 -Log out/no-launcher/controls-v4/focus-product-build.log -Target narutorise,native_engine_menu_test`; `REX_NATIVE_TEST_API=d3d12|vulkan native_engine_menu_test.exe out/no-launcher/controls-v4/focus-final-<api>.jsonl` (variável definida em PowerShell).
- Data/ambiente: retomada desta sessão, Windows.

### TEST-023 — Analógicos exclusivos da visualização durante o teste
- Requisitos: REQ-002, REQ-008, REQ-012.
- Etapas: STEP-010.
- Tipo: integração de navegação e preview SDL virtual.
- Procedimento/comando planejado: movimentar LS/RS/LT/RT na tela de zonas mortas; conferir gráficos sem alterar foco/categoria/jogador/valores; usar direcional para navegar e ajustar; sair com stick ainda segurado, exigir neutral antes de voltar a navegar por ele; captura de eixos continua funcional.
- Resultado: PASS
- Evidência: focus-final-*.jsonl, checks pad-preview-*: LS/RS/LT/RT reais da fixture SDL atualizam gráficos mantendo foco/categoria/jogador/configuração; direcional alcança ajuste e altera valor com stick segurado; B retorna à categoria; ao sair, stick ainda segurado fica suspenso e neutral restabelece navegação. Captura de seis eixos nos dois sentidos e B/LB/RB continuam passando no mesmo executável. Fixture isolada dos dispositivos físicos por hints SDL exclusivos do teste.
- Comandos executados: mesmo build/executável/APIs de TEST-022.
- Data/ambiente: retomada desta sessão, Windows.

## Validação da correção STEP-010
- `python -I app/tests/controls_boot_test.py app/out/build/win-amd64-release game out/no-launcher/controls-v4/focus-persistence both`: exit 0. Quatro processos (primeiro/reiniciado, D3D12/Vulkan) PASS. Entrada em Botões agora por teclado desde o foco inicial; capturas, reabertura, cancelamento, Del, restauração e isolamento persistem. Resultado em focus-persistence/results.json; log focus-persistence.log. Screenshots d3d12-initial.png e focus-final-d3d12.jsonl.deadzone-preview-1280x720.png inspecionadas; sem alteração do layout v4 já comparado ao FigCraft.
- Iterações intermediárias preservadas como diagnóstico: focus-fixed-d3d12.jsonl expôs busca breadth-first preferindo links finais aos campos (corrigida para ordem de documento); focus-fixed2-d3d12.jsonl teve somente o estímulo Y invertido do teste, corrigido para direção SDL positiva para baixo. Validação final completa passou nas duas APIs.
- Há um DualSense físico detectado via SDL nesta retomada; os testes de interação/rumble continuam usando somente fixtures. Detecção não comprova operação manual do hardware; dois controles físicos do mesmo modelo e câmera durante exploração continuam sem validação. TEST-011/012 também reexecutados pela suite integrada, sem falhas.
- Resultado atual: 17 casos PASS e 6 com partes físicas/manuais pendentes, conforme seus registros; nenhum código pendente de STEP-010. `python scripts/validate_tasks.py` passou, 1 task e zero problemas; diff --check no escopo da correção passou.
