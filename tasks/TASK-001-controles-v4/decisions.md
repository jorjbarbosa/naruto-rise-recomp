# Decisões — TASK-001

DEC-001 a DEC-008 aprovadas pelo usuário (“aprovado”), nesta sessão em 2026-10-25 após apresentação do plano. Execução na branch feat/no-launcher conforme steering do usuário.

### DEC-001 — Usar as quatro portas do SDK com atribuição explícita
- Estado: APPROVED
- Contexto: o SDK suporta quatro usuários, mas SlotAssignment decide por ordinal e soma sintéticos a J1.
- Alternativas: quatro abas sem efeito real; substituir todo o sistema de entrada; política configurável sobre o sistema existente.
- Justificativa: instalar política do app uma vez antes do polling e consultar snapshots imutáveis de configuração/atribuição. Um dispositivo por porta; atualizar snapshots de modo sincronizado, sem trocar assignment durante chamadas guest. NOP continua restrito a modos de ferramenta/compatibilidade fora das portas explicitamente configuradas como Nenhum.
- Impacto: REQ-003, REQ-005, REQ-017; STEP-002, STEP-003.
- Aprovação: usuário, sessão de 2026-10-25 (“aprovado”).

### DEC-002 — Migrar valores antigos sem manter atribuição múltipla oculta
- Estado: APPROVED
- Contexto: esquema antigo permite teclado e primeiro controle simultâneos em J1; novo requisito exige exclusividade.
- Alternativas: manter união escondida; apagar preferências; migrar bancos e escolher dispositivo inicial conforme configuração real.
- Justificativa: copiar key_binds, mnk_mouse e sensibilidade para o banco MnK de J1, mantendo alternativas. Quando mnk_mode=true, atribuir teclado/mouse a J1 e distribuir controles reais detectados pelas próximas portas livres; quando false, conservar a ordem dos controles nas portas. Portas sem dispositivo real ficam Nenhum. Novas portas recebem padrões reais do projeto. Efetuar a migração uma única vez, salvando somente com gravação bem-sucedida; não persistir um handle de conexão nem completar lista com controles fictícios. A antiga união deixa de existir e será explicada na documentação.
- Impacto: REQ-004, REQ-015, REQ-019; STEP-001, STEP-003.
- Aprovação: usuário, sessão de 2026-10-25 (“aprovado”).

### DEC-003 — Expor identidade, capacidades e telemetria bruta sem desbloquear o guest
- Estado: APPROVED
- Contexto: inventário SDK é privado, SDL público só expõe GUID de modelo, e GetDeviceState retorna neutralizado quando menu está ativo. MnK anuncia capacidades sintéticas.
- Alternativas: usar estado guest neutralizado; enumerar dois backends desconexos; ponte aditiva no SDK compartilhada com serviço do app.
- Justificativa: ampliar contratos com inventário/snapshots seguros, identidade SDL serial/path quando disponível, identidade XInput por slot e flags reais de recursos. Telemetria observada antes do gate/mapeamento e atualizada no thread apropriado; UI não consome fila global SDL nem chama enumeradores guest concorrentemente. MnK recebe perfil efetivo publicado pelo app, com compatibilidade para cvars legadas. Preservar callbacks e alterações locais existentes; registrar mudanças em patches/sdk reaplicáveis.
- Impacto: REQ-004, REQ-006, REQ-012, REQ-017, REQ-018, REQ-019; STEP-002, STEP-003.
- Aprovação: usuário, sessão de 2026-10-25 (“aprovado”).

### DEC-004 — Identidade persistente sem promessa de distinguir o indistinguível
- Estado: APPROVED
- Contexto: DeviceId é transitório; dois controles podem compartilhar modelo/GUID. XInput não fornece identidade física estável.
- Alternativas: assumir GUID único; escolher arbitrariamente por ordem; usar identidade mais forte e explicitar fallback.
- Justificativa: SDL usa serial e/ou path quando inequívocos, além de metadados de modelo; nomes iguais recebem sufixo estável durante a sessão. XInput exibe slot e salva seleção por slot. Se a identidade de um SDL salvo ficar ambígua após reconexão/reinício, manter ajustes e solicitar nova atribuição, sem transferir silenciosamente uma unidade já ocupada. UI deve indicar essa limitação, não inventar modelo/serial.
- Impacto: REQ-004, REQ-005, REQ-006, REQ-018; STEP-002, STEP-006.
- Aprovação: usuário, sessão de 2026-10-25 (“aprovado”).

### DEC-005 — Controlador RmlUi próprio e captura com estados explícitos
- Estado: APPROVED
- Contexto: Panel modela opções/descrição; a configuração nova tem categorias persistentes à esquerda e vínculos editáveis à direita. Captura atual por diálogo mistura teclas com comandos virtuais de controle.
- Alternativas: estender Panel com regras particulares de controles; documento/controller dedicado usando tema/ciclo de vida existentes.
- Justificativa: `menu/controls.*` e `ui/controls.rml/.rcss` separados, reutilizando fontes, recursos e estilos do projeto. Máquina de captura: inativo → aguardando liberação do iniciador → armado → aplicação/cancelamento/remoção → supressão até liberação. Guardar porta, tipo de banco e vínculo, sem ponteiros de elementos que possam ser recriados. Durante captura interceptar fontes brutas antes de PadInput/atalhos/RmlUi; Esc/Del físicos continuam comandos de captura, B/LB/RB continuam fontes. Trocas via mouse também ficam bloqueadas.
- Impacto: REQ-001, REQ-002, REQ-007, REQ-008, REQ-009, REQ-010; STEP-004, STEP-005.
- Aprovação: usuário, sessão de 2026-10-25 (“aprovado”).

### DEC-006 — Preservar política de conflitos existente no banco correto
- Estado: APPROVED
- Contexto: MnK atual mantém exclusividade de uma tecla retirando a mesma entrada de outros vínculos; gamepad não tem política de remapeamento anterior.
- Alternativas: alterar vários bancos ao remover duplicadas; mudar política de teclas silenciosamente; conservar política e limitar seu alcance.
- Justificativa: editar somente o vínculo escolhido e preservar alternativas; a única alteração adicional no banco MnK é a resolução de duplicata já prevista no projeto, restrita à mesma porta e tipo de banco. Não limpar outras portas ou mapa de controle. Em gamepad, sem política anterior, modificar somente o alvo; documentar múltiplos destinos da mesma entrada, se permitidos pelo novo modelo. Esc/Del reservados durante captura não se tornam vínculos de teclado; teclas previamente reservadas continuam explicadas na linha/rodapé, sem abrir tela de remapeamento.
- Impacto: REQ-008, REQ-009, REQ-010; STEP-001, STEP-005.
- Aprovação: usuário, sessão de 2026-10-25 (“aprovado”).

### DEC-007 — Transformações efetivas por porta e câmera sem vazamento entre portas
- Estado: APPROVED
- Contexto: sensibilidade, mouse e câmera direta são globais; não há zonas mortas editáveis ou mapas de pad. O hook de câmera do jogo não possui quatro câmeras.
- Alternativas: alterar cvars globais a cada aba; substituir hook por câmera nova; perfil por porta consumido pela entrada existente.
- Justificativa: bancos de pad e MnK separados; ações de saída Xbox 360, fontes tipadas (botão, eixo e sinal, tecla/modificadores, botão/movimento do mouse conforme suporte). Validar faixas normalizadas, eixos [-32768,32767], gatilhos [0,255], motores [0,65535] e sensibilidade [0.01,10.0]. Deadzones de LS/RS/LT/RT e enable por porta, com intervalo que evite denominador zero; usar a mesma transformação para preview efetivo e guest. Câmera direta somente quando o teclado é da porta que controla a câmera única existente (J1); em outras portas, entrada convencional pelos eixos guest. O hook respeita direção remapeada/removida, habilitação e sensibilidade do perfil efetivo, e não duplica delta do right stick.
- Impacto: REQ-009, REQ-011, REQ-012, REQ-013, REQ-017, REQ-018; STEP-001, STEP-003, STEP-007.
- Aprovação: usuário, sessão de 2026-10-25 (“aprovado”).

### DEC-008 — Uma transação para estado persistido e efetivo; rumble com proprietário e prazo
- Estado: APPROVED
- Contexto: transferência altera duas atribuições; restauração abrange dois bancos; SDK SDL usa duração longa para rumble do guest e XInput mantém efeito até nova chamada.
- Alternativas: salvar campos separadamente; atualizar UI antes de gravar; operação sobre cópia validada com publicação após autosave.
- Justificativa: reaproveitar gravação segura de Settings, preservando chaves desconhecidas. Transferência/restauração são transações únicas. Confirmação revalida porta, identidade e inventário antes de salvar. Rumble de UI tem dono (tela, porta, device generation), parada explícita e prazo máximo; zerar motores ao perder esse dono, inclusive fechar/perder foco/desconectar. Identificar não altera atribuição nem inicia efeito sem recurso físico.
- Impacto: REQ-005, REQ-006, REQ-014, REQ-015, REQ-016; STEP-001, STEP-006, STEP-007, STEP-008.
- Aprovação: usuário, sessão de 2026-10-25 (“aprovado”).

## Registro de implementação dentro do plano aprovado — 2026-10-25
- DEC-003/004: identidade SDL combina serial e caminho quando ambos existem, evitando colisão de serial entre unidades com paths distintos. A UI mantém snapshots próprios da leitura nativa no thread UI; SDK compartilha identidade e perfil, sem enumerar InputSystem concorrentemente a partir da UI.
- DEC-005/006: teclas equivalentes (Return/NumpadEnter e ordem/alias de modificadores) compartilham identidade de conflito. A regra existente de modificador standalone é respeitada e explicada na linha quando impede um chord. Captura limpa hover antigo para a entrada recém-capturada não mover foco na reconstrução da lista.
- DEC-007/008: estado/capacidades/keystrokes usam o mapa efetivo; eventos não carregam histórico de GetState e incluem diagonais/repetição. A saída nativa zera rumble antes/depois de parar guest para cobrir o caminho hard-exit.
- REQ-017/019: NRINPUT 2 conserva D/S/K e aceita anotação opcional P para porta, transferência e remoção. Arquivos antigos continuam SlotAssignment. Teste público da factory confirmou record/replay do driver MnK e relógios por tempo/leituras, inclusive lifetime após Shutdown. Sem mudança de modo multiplayer.
- Patches MnK foram consolidados no patch já existente para que aplicar a pilha novamente continue reconhecendo as mudanças; z-controls-v4.patch contém os demais contratos. Baseline fixada + patches reproduzem nove arquivos do SDK e os dez patches passam reverse --check.
