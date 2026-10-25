# Configurações no jogo

A tela de configuração e o overlay pertencem ao renderer nativo. As mesmas
páginas são usadas antes e durante o jogo. F1 abre o menu; no controle, pressione
Back + Start juntos. Os dois atalhos podem ser alterados em Controles.
O jogo continua rodando com o menu aberto: apenas a entrada do guest fica neutra,
inclusive teclado/mouse, XInput e replay. Ao fechar, botões ainda pressionados
precisam ser liberados antes de voltarem ao jogo.

Cada edição é salva no `narutorise.toml` junto do executável. Chaves adicionais
são preservadas; uma configuração inválida ou que não pôde ser gravada não é
substituída. A marca • identifica valores alterados nesta sessão.

| Opção | Quando aplica |
| --- | --- |
| Tela cheia | Ao vivo |
| Mostrar FPS | Ao vivo, também no nativo |
| Mostrar indicador de shaders | Ao vivo; inclui compilação em segundo plano e criações síncronas recentes |
| Idioma do menu | Ao vivo; sete idiomas |
| Atalho de teclado e combinação do controle | Ao vivo |
| Arquivo do jogo | Antes de Jogar; durante o jogo, salva e oferece reinício |
| API gráfica e placa | Salva e oferece Reiniciar agora / Depois; o reinício mantém a pasta de dados e volta à tela de configuração |
| Idioma do jogo e pacote de tradução | Próximo início do jogo |
| Pular vídeos | Nas próximas aberturas dos vídeos de introdução |
| Resolução interna, brilho, FPS alvo, vsync | Ao vivo, com confirmação após a aplicação |
| MSAA, FXAA, pós-processamento, filtragem e AA de transparências | Ao vivo, com confirmação após a aplicação |
| Backend de entrada SDL/XInput | Antes de Jogar ou próximo início do jogo |
| Dispositivo, mapas de controle/teclado, zonas mortas, câmera, sensibilidade e vibração por porta | Ao vivo, após autosave bem-sucedido |
| Latência e buffer triplo | Ao vivo; Vulkan mantém latência efetiva 1 e respeita os limites da superfície |
| DLC extraída ou pacote STFS preparado | Próximo início do jogo; cópias terminam antes de sair |

As opções que dependem de outro início exibem **REINICIA**. O Engine temporário
é fechado antes de reler as opções para criar o Engine do jogo. O Engine em
execução recebe alterações gráficas no limite entre quadros. O renderer conclui
o trabalho anterior da GPU antes de reconstruir alvos ou a apresentação,
preservando as texturas resolvidas. O menu confirma o novo valor após o aceite;
uma recusa restaura a configuração anterior. A troca de FPS reinicia o relógio
e o histórico da interpolação no começo de uma iteração completa da lógica.
Ao mudar a API gráfica ou a placa, **Depois** fica selecionado por padrão;
Enter ou Esc mantém a sessão atual e a opção salva para o próximo início.
**Reiniciar agora** pede uma escolha explícita e, durante o jogo, avisa sobre
o progresso desde o último salvamento.

A página Avançado também verifica atualizações e cria um atalho `.lnk`. O
atualizador apenas avisa e abre a página da release; não baixa nem instala a
versão. `check_for_updates = false` desliga a consulta automática. A consulta
manual continua disponível. O atalho aponta diretamente a `narutorise.exe`,
mantém o diretório de trabalho e a pasta de dados, e não fixa `--setup` nem o ISO.

Para testes isolados das boots, use `app/tests/setup_boot_test.py`; ele cria sua
própria cópia do produto e não altera o ISO fornecido.

O pré-lançamento segue o F0 v7: título/logo e menu centralizados, seleção laranja,
sem capa, kunai ou “Port para PC”. Antes de selecionar o jogo, mostra o título
em texto e Escolher o jogo; com dados válidos, mostra Jogar, Configurações, DLC
e Sair. Os comandos do rodapé acompanham teclado ou controle.

O logo é opcional e vem de `Naruto_clip.SRA` da ISO/pasta validada. O leitor em
`game_logo.cpp` resolve `6e0003ce_I15` pelo catálogo FRSA, limita as leituras e a
descompressão zlib, confere o checksum e converte DDS BC3 para RGBA. O worker não
acessa RmlUi ou GPU; a UI descarta resultados de origens anteriores e publica a
textura na própria thread. Arquivo ausente, truncado ou incompatível mantém o
título textual e não bloqueia Jogar. O fundo é procedural e nenhuma dessas artes
é embutida ou extraída para um cache em disco pelo produto.

`narutorise_game_logo_test` cobre parser, corrupção, alpha, pasta e ISO sintéticos;
pode receber caminhos adicionais de jogo/ISO para validar o logo local real.
`prelaunch_logo_boot_test.py PRODUCT GAME OUTPUT [ISO]` verifica captura sem jogo,
seleção pelo diálogo nativo, troca pelo logo e fallback em ambas as APIs. Sem ISO
fornecida, constrói um contêiner de teste apenas para o pré-lançamento, que não
deve ser usado para executar o jogo. Os testes usam cópias/configurações isoladas.

O prelaunch verifica também os arquivos essenciais do jogo. Uma pasta com apenas
`default.xex` e DLLs para recompilação não habilita Jogar. A busca automática
prefere uma instalação com os dados necessários, inclusive `game/` na raiz do
repositório ao executar o build de desenvolvimento.
`app/tests/prelaunch_data_boot_test.py` recebe produto, pasta extraída completa,
ISO e saída nova; verifica essa seleção e o carregamento após Jogar nas duas APIs,
com configuração e saves isolados.

Os painéis seguem o protótipo F0 v4. **Controles → Configurar controles** abre
a janela dedicada com Jogador 1 a 4, dispositivo/categorias à esquerda e
mapeamentos/ajustes à direita. Clique ou LB/RB troca a porta; uma categoria
compatível permanece aberta. A seleção da categoria é azul; o foco tem contorno
claro. A linha laranja indica o vínculo em captura.

A/Enter ou seta direita na categoria entra nos campos; B/Esc devolve o foco à
categoria e, a partir dela, fecha a janela. As setas/direcional percorrem os
campos e alternativas. No teste de zonas mortas, os sticks alimentam somente
os gráficos: navegue pelo direcional. Ao sair do teste, solte o stick antes de
voltar a usá-lo na navegação.

Ao selecionar um vínculo, somente seu valor muda para **Pressione uma entrada…**.
Solte a entrada que iniciou a captura, pressione a nova entrada, use Esc para
cancelar ou Del para remover. A lista permanece visível. Enquanto captura e
aguarda a liberação final, navegação, atalhos de menu, categorias, abas e ajustes
ficam suspensos; B/LB/RB são entradas capturáveis. Clique direito e X são entradas,
sem limpar outros campos. Alternativas de teclas são preservadas; uma entrada
duplicada sai somente dos outros vínculos do mesmo banco e porta.

Um dispositivo pertence a uma porta. Escolher um dispositivo ocupado exige
confirmação: transferir deixa a origem sem dispositivo e conserva os ajustes de
ambas as portas. Nenhum desconecta efetivamente a porta; o fallback NOP não é
mostrado como hardware. Conexões e desconexões atualizam a lista, invalidam
confirmações obsoletas e cancelam a captura do dispositivo removido.

Cada porta guarda bancos separados de controle e teclado/mouse em
`[controls.player1]` a `[controls.player4]`, esquema `controls.version = 1`.
Arquivos antigos conservam suas alternativas e ajustes de teclado em J1. Se
`mnk_mode` estava ligado, teclado/mouse ocupa J1 e os controles detectados ocupam
as próximas portas livres; caso contrário, os controles mantêm a ordem inicial.
Os campos globais legados são preservados para compatibilidade, mas o esquema
por porta passa a ser a fonte de entrada do menu nativo. As quatro portas não
adicionam modos multiplayer ao jogo.

SDL combina serial e caminho reais quando ambos existem; quando só há identidade de sessão, a atribuição
precisa ser refeita após reiniciar. XInput identifica slots, sem garantir qual
unidade física ocupará cada slot ao reconectar. Nomes iguais recebem sufixos e
Identificar vibra brevemente quando houver suporte. Identidades ambíguas não são
resolvidas por suposição.

As zonas mortas ignoram pequenas entradas involuntárias perto do repouso. Cada
analógico e gatilho tem seu valor (0–95%) e há um toggle de aplicação. Os gráficos
mostram entrada real mapeada antes do filtro, região ignorada e saída efetiva;
não há simulação. Teclado/mouse não oferece zonas mortas ou vibração. A
sensibilidade aceita 1–1000%, conforme o SDK. A câmera direta respeita seus
vínculos de mouse e só atende J1; em outra porta, o mouse usa os eixos guest.

Vibração tem intensidade por canal/motor e teste com Parar e prazo de cinco
segundos. Sair, trocar jogador/categoria/dispositivo, perder foco, fechar ou
desconectar encerra o teste. Após desconectar, a UI invalida o efeito; o backend
já removido pode não aceitar outro comando de motor. Restaurar padrão pede
confirmação e restaura ambos os bancos e ajustes apenas da porta selecionada,
mantendo o dispositivo e os atalhos globais.

O aviso do atalho aparece no topo central por 2,5 segundos, após a abertura do
jogo, separado das notificações de atualização. O indicador de shaders mostra
o progresso do lote de pré-compilação ou a atividade sem total conhecido.

Uma tecla modificadora não pode ficar simultaneamente isolada e numa combinação
no mesmo banco. A captura verifica as duas ordens de cadastro e o conjunto que
resultaria da edição, sem alterar vínculos quando há conflito. Substituir o
próprio vínculo é permitido quando deixa de haver conflito.

O marcador de abertura só é removido depois de um Present do jogo aceito com
sucesso pelo backend. Pedir/desenhar a UI, apresentar o Engine temporário, falhar
ou ignorar uma janela vazia/minimizada não confirma a abertura. A confirmação é
única e volta para a thread da UI antes de remover o arquivo.

`app/tests/menu_visual_test.py` captura os painéis e o HUD nas duas APIs,
inclusive em tela larga. `close_boot_test.py` verifica Alt+F4 real no jogo e
nos painéis. `dlc_boot_test.py` instala conteúdo de teste pelo seletor nativo.
Os três recebem pasta do produto, ISO e uma pasta nova de resultados, e usam
configuração e dados isolados em Temp.

`controls_boot_test.py` recebe produto, jogo/ISO, saída nova e opcionalmente
`d3d12`, `vulkan` ou `both`. Testa autosave, reabertura, novo processo, Esc/Del,
restauração e isolamento, e captura 720p/1080p/21:9. Os targets
`narutorise_controls_{config,service,runtime,capture,replay}_test` e
`native_engine_menu_test` cobrem modelo, backend, captura, transferência e
lifetime. Controles SDL virtuais são criados somente nos testes. Prova física
de rumble, drift e reconexão exige hardware; resultados ficam na task.

Gravação/replay conserva o formato `NRINPUT 2`: anotações opcionais `P` registram
porta, transferência e desconexão. Arquivos antigos sem essas anotações conservam
a atribuição original. O replay usa as portas gravadas e não passa a listar seus
dispositivos como hardware real. O fechamento nativo zera os motores antes e
depois de parar o guest, pois o SDK pode encerrar sem executar `OnShutdown`.
