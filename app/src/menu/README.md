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

A página Avançado agrupa as opções em Geral (menu inicial, atualizações e
atalho), Indicadores na tela, Desempenho, Diagnóstico (envio de relatórios e
diagnósticos locais) e Pasta de dados. Ela verifica atualizações e cria um atalho `.lnk`. O
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

O logo é opcional e vem de `Naruto_clip.SRA` da ISO/pasta validada. O leitor comum
em `game_assets.cpp`, usado por `game_logo.cpp` para `6e0003ce_I15`, resolve os nomes
pelo catálogo FRSA (tudo ou nada), limita as leituras e a descompressão zlib, confere
o checksum e converte DDS BC1/BC3 para RGBA. O worker não
acessa RmlUi ou GPU; a UI descarta resultados de origens anteriores e publica a
textura na própria thread. Arquivo ausente, truncado ou incompatível mantém o
título textual e não bloqueia Jogar. O fundo é procedural e nenhuma dessas artes
é embutida ou extraída para um cache em disco pelo produto.

`narutorise_game_logo_test` cobre parser, corrupção, alpha, pasta e ISO sintéticos;
pode receber caminhos adicionais de jogo/ISO para validar o logo local real.
`narutorise_game_assets_test` cobre a busca de vários nomes (ordem, ausência,
duplicidade), BC1 com quatro e três cores, BC3 e entradas corrompidas; com caminhos
de jogo/ISO, confere as 13 imagens do menu no jogo (`kMenuImages`).

Aberta pelo Options do próprio jogo, Configurações imita essa tela
(`game_style_panel.cpp`, `ui/game_menu.rml`): papel, cortinas, faixa do título,
nuvem do cursor, pincéis do brilho e botões vêm das 13 imagens, lidas num worker
quando o jogo começa; os textos usam Edo SZ (licença Vic Fieger, embutida). Até
a arte chegar, ou se ela falhar, a página usa o painel comum. A tela de
configuração, a barra do overlay, a DLC e o que abre pelo F1 continuam no
estilo próprio. O russo usa Roboto Condensed, pois a Edo SZ não tem cirílico.
Rótulos e valores longos encolhem até caber, e as opções liga/desliga usam
palavras curtas (On/Off, Sim/Não, Oui/Non…), como o YES/NO do jogo.

A página foi medida 1:1 na tela Options do próprio jogo, a 1280×720 e quadro a
quadro a 60 fps. As referências estão em `out/menu-jogo-study-20261030/reference`.
Os textos usam o tamanho e as cores do movie. A borda reproduz as cópias
deslocadas que o jogo desenha sob cada texto: os rótulos e as abas sobre quatro
cópias, o rodapé sobre quatro cópias pretas e os valores sobre uma sombra. Ela é
recalculada para a escala da janela, porque os efeitos de fonte do RmlUi são em
pixels inteiros. O YES/NO tem o círculo atrás da primeira letra, e a barra não
mostra número. A faixa e os painéis passam sob as cortinas; a nuvem do cursor
fica por cima. A página ocupa a mesma imagem 16:9 em que o renderer nativo
apresenta o jogo, com as faixas pretas fora dela em janelas de outra proporção
(21:9, por exemplo), e nada que entra ou sai passa dessa imagem. As cortinas
têm 724 de altura e começam 1,5 acima, como no filme, para que suas linhas
esfumadas fiquem fora da tela.

As animações (`game_style_motion.cpp`) usam as curvas medidas, avançando dois
quadros do filme a cada 1/30 s: mesmo com 60 fps ou mais, os menus do jogo
avançam no passo da lógica, como no console.
- **Abrir:** o papel aparece de uma vez. As cortinas entram pelas laterais e
  cada rótulo desliza da esquerda, uma linha a cada quatro quadros. A faixa do
  título, os painéis com os valores e o rodapé chegam em seguida, e a mancha de
  tinta cresce por último.
- **Fechar:** o papel some e o jogo aparece atrás. As linhas saem uma por
  quadro, seguidas dos painéis, da faixa, do rodapé e das cortinas.
- **Trocar de aba:** só as linhas e os painéis saem e entram.
- **Seleção:** o rótulo focado pulsa na horizontal (4%, a cada 50 quadros) e a
  nuvem salta como mola ao mudar de linha.

Voltar (Esc, B, BACK) toca a saída antes de fechar. Qualquer tecla ou clique
termina a animação. Teclas que chegam enquanto os painéis saem, ou com uma ação
ainda pendente, esperam e são aplicadas em ordem na página seguinte, de modo que
LB/RB e as setas em sequência rápida não se perdem. Só mudam margens, tamanhos e
opacidade, e a escala fica nos rótulos e na nuvem, para que a lista continue
recortada pelo scissor. ←/→ alternam ON/OFF, como as demais escolhas, igual ao
painel comum.

O Options do próprio jogo, no menu principal ou na pausa, abre estas
Configurações no lugar da tela dele, com uma aba **Opções** primeiro: volumes de
música, vozes e efeitos, câmera invertida nos dois eixos, vibração, legendas e,
nos menus de título com o conteúdo instalado, as vozes em japonês. O brilho do
jogo fica fora (vale o nosso, em Tela). Pelo F1 o estilo do jogo não aparece:
as Configurações usam o painel dos menus e a janela de controles. Configurar
controles, aberto desta página, continua no palco do jogo (`game_style_controls.cpp`,
`ui/game_controls.rml`): os jogadores na faixa do título, as categorias à
esquerda e a página à direita, sobre o mesmo modelo de `controls.cpp`. Uma
lista maior que a área mostra a barra de rolagem no papel, ao lado dos painéis,
e cópias do papel cobrem as linhas que não cabem inteiras. Com
`game_options_settings = false` (ou `--game_options_settings=false`), o Options
do jogo continua com a tela original. Como no original, aberto pela pausa o
Options volta a tocar a música.
`game_options_script.cpp`, compilado em cada módulo AI2C (o do jogo e os três
das DLCs), envolve o estado do Options: quando ele mostra o filme, o gancho
esconde o filme e, nos menus de título, o papel (na pausa o papel é o do menu de
pausa e fica), lê as opções e avisa o app. Cada mudança passa pelo
`fn_Option_SetValue` do jogo no quadro seguinte, então volume e vibração valem na
hora. Ao fim da saída da página, o filtro de entrada aperta o A do jogo, que
salva as opções e volta ao menu dele como na tela original; o gancho termina de
esconder o filme e não deixa filme na tela, para o próximo menu não esperar a
saída de um filme escondido. O app guarda a sessão (`game_options.h`) e a
exporta por nome (`narutorise_game_options`), porque os módulos são bibliotecas
à parte.

Com `NARUTORISE_TEST_GAME_ROOT`, `native_engine_menu_test` captura as cinco abas
e a aba Opções nos sete idiomas, confere que nada transborda e verifica os botões
do controle. Com um relógio próprio, captura quadros de abrir, trocar de aba e
fechar, e confere que a entrada pula a animação. `game_style_boot_test.py PRODUCT
GAME OUTPUT` abre o jogo nas duas APIs, entra no Options do jogo, muda a música e
confere que o jogo guardou o valor ao reabrir; depois captura a abertura, as
cinco abas, a troca de aba e o fechamento em 720p, além de 1080p e 2560×1080. As
teclas são entrada real na janela em foco, então a área de trabalho precisa
ficar livre.
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
