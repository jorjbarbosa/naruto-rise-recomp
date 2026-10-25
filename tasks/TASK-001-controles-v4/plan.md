# Plano — TASK-001

Status: DONE — implementação executada após aprovação do usuário (“aprovado”, sessão de 2026-10-25), na branch feat/no-launcher. Critérios automatizáveis verificados; limites físicos/manuais em tests.md.

Executar um STEP por vez, atualizando progress/tests/decisions/handoff ao encerrar. Antes de iniciar: reler os sete documentos, confirmar aprovação registrada, Git/diffs e testes reais. Não implementar nenhum item deste plano enquanto a aprovação estiver pendente.

## Resultado e integração propostos
Modelo versionado de quatro portas em narutorise.toml, com atribuição exclusiva e bancos separados de controle/MnK. Serviço do app publica snapshots imutáveis para UI e runtime; uma política de atribuição instalada antes do guest consulta esse estado. Ponte aditiva no SDK fornece identidade/capacidades/telemetria bruta e perfil MnK efetivo. Documento RmlUi dedicado reproduz o protótipo e captura fontes antes da tradução de navegação. Autosave só publica alterações após gravação; confirmações revalidam seus alvos.

Incompatibilidades e escolhas para aprovação constam em DEC-001–DEC-008: especialmente migração da antiga união teclado+controle em J1, limitações de identidade XInput/SDL sem serial e Nenhum realmente desconectado. Não migrar backend silenciosamente.

### STEP-001 — Modelo de quatro portas e persistência versionada
- Objetivo: representar atribuições, fontes tipadas, dois bancos e ajustes por porta, migrando arquivos antigos sem perda de valores.
- Requisitos: REQ-003, REQ-009, REQ-010, REQ-011, REQ-013, REQ-014, REQ-015, REQ-016, REQ-019.
- Testes: TEST-001, TEST-002, TEST-003.
- Arquivos previstos: novos `app/src/controls/config.h/.cpp`; `launcher/src/config_manager.h/.cpp`, `launcher/src/key_bindings.h/.cpp` apenas onde necessário para compatibilidade; `app/src/menu/settings.h/.cpp`; `app/tests/controls_config_test.cpp`; `app/CMakeLists.txt`.
- Dependências: nenhuma.
- Mudanças: esquema versionado com quatro perfis, fontes sem vínculo, alternativos MnK preservados, ajustes validados e save transacional. Migração DEC-002 copia valores legados; a resolução de dispositivos reais ocorre quando o inventário estiver pronto. Preparar operações puras para editar, transferir e restaurar mantendo o dispositivo, sem expor a UI nova ainda.
- Critério de pronto: testes de round-trip, migração, limites, isolamento, duas alternativas, cancelamentos e falha de gravação passam com diretórios temporários; chaves alheias preservadas.
- Riscos: sobrescrever configuração antiga, defaults incompatíveis com teclas já reservadas, perda de alternativas ou publicação parcial.

### STEP-002 — Inventário, identidade e leitura bruta do backend
- Objetivo: fornecer uma fonte real de dispositivos/capacidades/telemetria à UI antes e durante o jogo.
- Requisitos: REQ-004, REQ-006, REQ-012, REQ-017, REQ-018, REQ-019.
- Testes: TEST-004, TEST-005.
- Arquivos previstos: novos `app/src/controls/service.h/.cpp`; `app/src/menu/pad_input.h/.cpp`, `app/src/menu/ui.h/.cpp`; `sdk/include/rex/input/device.h`, `input_driver.h`, `input_system.h`; implementações SDL/XInput e `sdk/src/input/input_system.cpp`; patch SDK específico em `patches/sdk/`; testes de serviço/driver e `app/CMakeLists.txt`.
- Dependências: STEP-001.
- Mudanças: ponte de snapshots sincronizados, SDL serial/path/handle de sessão, XInput slot, capacidades físicas e leituras antes do gate. Polling/eventos e fechamento no thread correto, sem consumir fila SDL compartilhada. Inventário respeita backend selecionado e exclui NOP/replay da lista física de produção. Detectar hotplug e invalidação por geração.
- Critério de pronto: inventário/telemetria não dependem de guest ativo; estados reais disponíveis com menu aberto; testes isolados de duplicatas, stale handles, capacidades e hotplug passam; limitações de identidade documentadas.
- Riscos: corrida entre thread UI e guest, lifetime SDL, dois inventários divergentes ou nova identificação confundida com dispositivo removido.

### STEP-003 — Roteamento efetivo, remapeamento e perfil MnK por porta
- Objetivo: fazer as quatro portas afetarem realmente todas as operações de entrada e feedback.
- Requisitos: REQ-003, REQ-009, REQ-010, REQ-011, REQ-013, REQ-017, REQ-018, REQ-019.
- Testes: TEST-006, TEST-007, TEST-008.
- Arquivos previstos: novos `app/src/controls/runtime.h/.cpp`; `app/src/input_replay.cpp`, `app/src/menu/input_gate.cpp`, `app/src/narutorise_app.h`, `app/src/mouse_camera_script.cpp`; `sdk/include/rex/input/mnk/mnk_input_driver.h`, `sdk/src/input/mnk/mnk_input_driver.cpp`; patches SDK correspondentes; `app/tests/controls_runtime_test.cpp`, `sdk/tests/unit/input/input_assignment_test.cpp`, `app/CMakeLists.txt`.
- Dependências: STEP-001, STEP-002.
- Mudanças: política explícita instalada antes do guest, snapshots seguros para atribuição/mapeamento/ajustes, perfil MnK da porta proprietária e transformações tipadas. Fazer GetState/GetCapabilities/GetKeystroke/SetState concordarem; Nenhum não ganha NOP escondido. Preservar gate/latch/record/replay e integrar câmera conforme DEC-007.
- Critério de pronto: testes de entrada e vibração observam só porta/dispositivo esperados, inclusive após transferência e mudanças ao vivo; teclado em J2 não altera mapa nem câmera de J1; fontes removidas ficam neutras; regressões do gate passam.
- Riscos: keystrokes antigos ainda usarem mapas físicos, câmera direta ignorar remoções, dados globais concorrentes, jogo reagir à ausência real em J1.

### STEP-004 — Estrutura visual v4 e percursos de cada banco
- Objetivo: construir janela dedicada com quatro abas, categorias à esquerda e conteúdo próprio à direita.
- Requisitos: REQ-001, REQ-002, REQ-003, REQ-010, REQ-018, REQ-019.
- Testes: TEST-009, TEST-010.
- Arquivos previstos: novos `app/src/menu/controls.h/.cpp`, `app/ui/controls.rml/.rcss`; `app/src/menu/settings.cpp`, `ui.h/.cpp`, `assets.cpp`, `text.cpp`; ajustes pontuais em `app/ui/theme.rcss` se reutilizáveis; `app/src/menu/tests/ui_test.cpp`, `app/CMakeLists.txt`.
- Dependências: STEP-001, STEP-002, STEP-003.
- Mudanças: acesso Configurar controles, título/voltar/fechar, J1–J4 por clique/LB/RB, separação categoria/foco/edição e percursos Botões ↔ Direcional/cliques, Gatilhos, Analógicos ou Movimento/câmera, Vibração, Zonas mortas e Restaurar. Desabilitar recursos com motivo quando não se aplicam. Preservar acesso a backend e atalhos reais, sem importar opção demonstrativa sem suporte.
- Critério de pronto: navegação completa nos dois bancos, quatro abas, return paths corretos, sete idiomas sem chaves cruas; screenshots em 720p/1080p/wide sem clipping ou overflow funcional.
- Riscos: painel genérico absorver foco/atalhos; mudança involuntária de banco ao retornar; layouts longos em outros idiomas.

### STEP-005 — Captura na linha e consumo completo da entrada
- Objetivo: substituir captura antiga do fluxo redesenhado por captura bruta na linha com liberação antes/depois.
- Requisitos: REQ-007, REQ-008, REQ-009, REQ-010, REQ-015, REQ-018.
- Testes: TEST-011, TEST-012, TEST-013.
- Arquivos previstos: novos `app/src/controls/capture.h/.cpp`; `app/src/menu/controls.h/.cpp`, `ui.h/.cpp`, `pad_input.h/.cpp`, `settings.cpp`, `text.cpp`; `app/ui/controls.rcss`; `app/tests/controls_capture_test.cpp`, testes RmlUi e `app/CMakeLists.txt`.
- Dependências: STEP-003, STEP-004.
- Mudanças: estados de captura DEC-005, atualizando somente valor da linha e rodapé. Interceptar teclas/mouse/controle antes de navegação/atalhos e distinguir Esc/Del físicos de comandos traduzidos. Cancelar sem salvar, Del remove, capturar B/LB/RB, botão/eixo/gatilho/direcional/clique/tecla/mouse; resolver conflitos existentes só dentro do banco correto. Não criar diálogo/página Remapear entrada e não reutilizar o diálogo antigo neste fluxo.
- Critério de pronto: lista e posição mantidas durante captura; testes comprovam bloqueio inclusive por clique nas abas/categorias/ajustes, iniciador não capturado e entrada recém-capturada não navega no término, cancelamento e repetição.
- Riscos: tratar B como Esc, LB/RB trocar jogador, clique inicial virar LMB, keyup gerar click atrasado, polling múltiplo avançar a captura duas vezes.

### STEP-006 — Escolha, transferência e hotplug na interface
- Objetivo: ligar lista e confirmações ao inventário real e operações transacionais.
- Requisitos: REQ-003, REQ-004, REQ-005, REQ-006, REQ-015, REQ-018.
- Testes: TEST-004, TEST-005, TEST-014.
- Arquivos previstos: `app/src/menu/controls.h/.cpp`, `app/src/controls/service.*`, `config.*`, `runtime.*`, `app/src/menu/text.cpp`, `app/ui/controls.rcss`; `app/tests/controls_assignment_test.cpp` e testes UI.
- Dependências: STEP-002, STEP-003, STEP-004, STEP-005.
- Mudanças: lista real com status, nomes repetidos distinguíveis, teclado/mouse e Nenhum. Confirmar transferência preservando ajustes da origem e destino; cancelar não grava. Revalidar identidade/proprietário antes de aplicar, atualizar desconexão e não entregar handles obsoletos a captura/rumble. Mostrar fallback/limitação quando identidade não puder ser recuperada com segurança.
- Critério de pronto: atribuição/transferência/cancelamento e desconexão em confirmação testados entre todas as portas; UI e runtime usam as mesmas atribuições; nenhuma duplicação ou transferência silenciosa.
- Riscos: confirmação antiga transferir um controle substituído ou salvar duas portas em operações separadas.

### STEP-007 — Zonas mortas, leitura real, sensibilidade e vibração
- Objetivo: integrar ajustes e feedback ao que o dispositivo realmente entrega e suporta.
- Requisitos: REQ-006, REQ-011, REQ-012, REQ-013, REQ-014, REQ-015, REQ-018.
- Testes: TEST-007, TEST-008, TEST-015, TEST-016.
- Arquivos previstos: `app/src/menu/controls.*`, `app/src/controls/service.*`, `runtime.*`, `config.*`, `app/src/menu/ui.cpp`, `text.cpp`, `app/ui/controls.rml/.rcss`; testes de transformação/rumble e UI.
- Dependências: STEP-002, STEP-003, STEP-004, STEP-005, STEP-006.
- Mudanças: explicação, toggle e valores LS/RS/LT/RT; gráficos de entrada bruta/região ignorada/resultado real, sem simulação do protótipo. Sensibilidade válida e intensidades por motor/porta. Identificar breve, iniciar/parar teste e expiração; zerar motores em toda saída, troca, perda de foco e desconexão. Desabilitar ajustes enquanto teste ativo conforme protótipo e reportar falha real.
- Critério de pronto: preview corresponde à transformação efetiva nos limites e ao mudar enable; sem visualizações inventadas ou polling guest destrutivo; mock de backend comprova parada/erro/lifetime e hardware disponível comprova leitura/rumble fisicamente.
- Riscos: mostrar sempre zero por ler depois do gate, rumble permanecer ativo no XInput ou competir com rumble guest, drift impedir armar captura.

### STEP-008 — Restauração confirmada e coerência após reabrir/reiniciar
- Objetivo: finalizar restauração de dois bancos, autosave e estado exibido em todo o ciclo.
- Requisitos: REQ-003, REQ-010, REQ-014, REQ-015, REQ-016, REQ-019.
- Testes: TEST-002, TEST-003, TEST-017, TEST-018.
- Arquivos previstos: `app/src/menu/controls.*`, `settings.*`, `app/src/controls/config.*`, `runtime.*`, `app/src/narutorise_app.h`, `app/src/menu/text.cpp`; testes de configuração, UI e novo `app/tests/controls_boot_test.py`.
- Dependências: STEP-001, STEP-003, STEP-004, STEP-005, STEP-006, STEP-007.
- Mudanças: confirmação com alcance explícito, restaurar pad/MnK/ajustes só da porta mantendo dispositivo, cancelar sem alteração. Recarregar estado persistido/effective após fechar/reabrir e reiniciar aplicação, inclusive falha de gravação e configuração legada. Atalhos globais continuam fora da restauração da porta.
- Critério de pronto: testes de conteúdo TOML e estado runtime/UI comprovam restauração isolada, dispositivo mantido, persistência em novo processo e nenhuma gravação em cancelamentos; testes não usam configuração pessoal.
- Riscos: dados iniciais da sessão sobreporem estado salvo, reset apagar dispositivo ou mapas das outras portas, save genérico apagar o novo bloco.

### STEP-009 — Validação integrada, comparação visual e documentação final
- Objetivo: verificar o fluxo inteiro e entregar evidências com limites de hardware explícitos.
- Requisitos: REQ-001, REQ-002, REQ-003, REQ-004, REQ-005, REQ-006, REQ-007, REQ-008, REQ-009, REQ-010, REQ-011, REQ-012, REQ-013, REQ-014, REQ-015, REQ-016, REQ-017, REQ-018, REQ-019.
- Testes: TEST-001, TEST-002, TEST-003, TEST-004, TEST-005, TEST-006, TEST-007, TEST-008, TEST-009, TEST-010, TEST-011, TEST-012, TEST-013, TEST-014, TEST-015, TEST-016, TEST-017, TEST-018, TEST-019, TEST-020, TEST-021.
- Arquivos previstos: `app/tests/controls_boot_test.py`, `controls_replay_test.cpp`, `menu_visual_test.py`, `menu_input_test.cpp`, `app/src/menu/tests/ui_test.cpp`, `app/src/menu/README.md`, `docs/configuration.md`, `patches/sdk/`, sete documentos desta task.
- Dependências: STEP-001, STEP-002, STEP-003, STEP-004, STEP-005, STEP-006, STEP-007, STEP-008.
- Mudanças: atualizar testes de percursos que dependem da UI antiga, executar build/testes do código novo, boots isolados, comparar telas com MCP FigCraft atual em D3D12/Vulkan nas resoluções previstas; revisar reaplicação dos patches SDK e diffs alheios preservados. Executar casos físicos somente com hardware disponível; identificar honestamente lacunas restantes.
- Critério de pronto: todos os testes automatizáveis necessários passam; validação visual registrada e sem telas/bancos incorretos; resultados físicos separados dos simulados e bloqueios documentados; documentação de uso/migração/limitações atualizada. Não declarar PASS de hardware que não foi testado.
- Riscos: binários antigos confundidos com novo build, teste automatizado dependente de posição antiga, falta de controles com rumble/mesmo modelo ou de backend GPU/ISO.

### STEP-010 — Corrigir acesso por teclado/controle e consumo dos analógicos no preview
- Objetivo: alcançar os campos da categoria sem mouse e observar os analógicos sem navegar involuntariamente.
- Requisitos: REQ-001, REQ-002, REQ-008, REQ-010, REQ-012.
- Testes: TEST-022, TEST-023, TEST-011, TEST-012.
- Arquivos previstos: app/src/menu/controls.*, pad_input.*, ui.cpp, controls_text.cpp, app/ui/controls.rcss, app/src/menu/tests/ui_test.cpp, app/tests/controls_boot_test.py e documentos da task.
- Dependências: STEP-009.
- Mudanças: seleção encaminha foco ao conteúdo; navegação nos widgets e retorno à categoria acessíveis por eventos reais; durante preview, eixos alimentam visualização e direcional continua navegando; ao sair, aguardar liberação de eixos ainda segurados antes de reativar navegação por stick. Manter a captura bruta e seus bloqueios.
- Critério de pronto: testes entram/saem das categorias e alteram valores sem injetar foco nos campos; SDL virtual demonstra preview ativo com foco/valor estáveis e direcional funcional; captura e B/LB/RB continuam corretos.
- Riscos: confirmação inicial ativar vínculo imediatamente, hover roubar foco, interferir na captura ou manter bloqueio de navegação fora do teste.
- Autorização: correção dos requisitos do plano aprovado em resposta ao defeito reportado nesta sessão, sem mudança de finalidade.
