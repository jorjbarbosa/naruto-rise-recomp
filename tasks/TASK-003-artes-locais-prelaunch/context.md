# Contexto — TASK-003

## Objetivo
Verificar artes locais e implementar o pré-lançamento com logo original carregado da cópia do usuário. A primeira fase foi a investigação descrita abaixo; após esboço F0 v7 e ajuste DLC, o usuário autorizou em 2026-10-25: “pode implementar a tela e a troca com o asset”.

## Implementação entregue — 2026-10-25
- F0 v7 implementado: título/logo e menu centralizados, Jogar/Configurações/DLC/Sair, primeira abertura com Escolher o jogo, fundo procedural, sem kunai/PORT PARA PC/capa embutida.
- `app/src/menu/game_logo.cpp`: FRSA por nome/tipo/chave, limites de tabelas/payload/dimensões, zlib limitado pelo stb existente, checksum Adler32, DDS BC3→RGBA, adaptadores pasta e ISO com verificação do extent mapeado. Asset opcional: falha mantém texto e não impede o jogo.
- `setup_screen.*`, `ui.cpp`, `render_interface.*`, `narutorise_app.h`: origem validada no estado, worker CPU sem acesso a UI/GPU, polling, rejeição de resultado obsoleto e textura dinâmica na thread UI. Removidos os embeds antigos de capa, fade, espiral e kunai de assets.cpp.
- Evidências: `out/prelaunch-implementation-2026-10-25/`. Leitor real em pasta/ISO produz pixels idênticos entre si e ao PNG da investigação: SHA-256 RGBA 9667da07656c583e33b6cc5af7a77b76f12c95befb96799cbc34507b97b6e5f4 (608.256 bytes).
- Build Release do produto concluído. 859 checks GPU/UI por API passaram, inclusive alpha, foco e descarte tardio; 8 casos de boot do logo e 15 regressões de setup passaram, incluindo ISO real, handover e sete idiomas.
- As seções seguintes registram a investigação anterior e seus limites naquela fase; a integração e os testes runtime acima foram executados posteriormente, sob autorização.

## Conclusão confirmada
**Sim, há imagens adequadas, extraídas e inspecionadas visualmente. O arquivo decisivo é `game/Naruto_clip.SRA`, não apenas `naruto.bf`.** As imagens de interface selecionadas são DDS padrão DXT5/BC3 ou DXT1/BC1 comprimidos com zlib. É possível localizar e descomprimir uma textura individual sem executar o jogo, sem interpretar ActionScript e sem carregar o BF inteiro.

## Arquitetura e comportamento confirmado
- Branch `feat/no-launcher`, HEAD `4355dd2364cf0322113b25a158dac134841e25f8`; numerosas modificações locais anteriores em app, SDK, packaging e docs foram preservadas.
- `naruto.bf`: 626.597.896 bytes, assinatura BIG, versão 42, 812 entradas e 2 pastas no índice. Foram lidos nomes, chaves e offsets, e descomprimidos os pacotes StartMenu, GameInterface, Common e MenuBoot em sondas locais.
- BF GameInterface: 63 blocos descomprimidos, 8.199.402 bytes, 32 marcadores KK2D. Common: 77 blocos, 10.078.114 bytes, 106 KK2D. A inspeção de formatos já existia em tools/native-renderer/inventory-bf-textures.py; o inventário por assinatura não comprova conteúdo visual por si só.
- BF `saveicon.bin`, chave FC000008: PNG 64×64, confirmado visualmente como ícone da raposa; inadequado como arte grande do Naruto.
- `Naruto_clip.SRA`: 69.643.872 bytes; assinatura FRSA, versão 1. Índice principal com 3.769 registros, dos quais 861 tipo 1 (clips) e 2.908 tipo 2 (texturas). Diretório de nomes com 3.756 registros: 861 clips e 2.895 texturas nomeadas. A diferença de 13 texturas permanece registrada, sem inventar nomes.
- Clips encontrados: Menu/PressStart (chave 6E0003CE), Menu/Menu (6E0003C4), PLV/Naruto_01_AP (8D0121D5), Menu/VisualReward/Character1 (A300086B) e Menu/VisualReward/Background1 (A300085F), entre outros.
- Menu/PressStart referencia 15 imagens, cujos nomes incluem 6e0003ce_I13. Referências de clip usam registros de 72 bytes; essa associação foi conferida diretamente.
- As texturas foram obtidas por leituras nos offsets do catálogo, zlib, conferência do tamanho declarado, validação DDS, decodificação BC1/BC3 para RGBA e visualização PNG. Não houve geração por IA nem retoque das artes.

## Candidatos visualmente confirmados
| Nome no diretório SRA | Dimensões | Conteúdo | Formato |
|---|---|---|---|
| 6e0003ce_I13 | 452×624 | Naruto em pose de chute, fundo transparente | DDS DXT5 |
| a300086b_I1 | 1280×724 | Naruto apontando, contorno vermelho, com transparência | DDS DXT5 |
| 8d0121d5_I1 | 352×432 | Retrato frontal de Naruto, transparência | DDS DXT5 |
| 6e0003ce_I1 | 1280×720 | Céu azul com luz solar | DDS DXT5 |
| 6e0003ce_I3 | 2356×372 | Faixa de nuvens com transparência | DDS DXT5 |
| 6e0003ce_I6 | 2440×668 | Segunda camada de nuvens com transparência | DDS DXT5 |
| 6e0003ce_I26 | 704×720 | Rosto de Naruto como efeito luminoso azul | DDS DXT5 |
| a300085f_I1 | 1280×724 | Fundo de explosão/luz laranja de recompensa | DDS DXT1 |
| 6e0003ce_I15 | 528×288 | Logo completo Naruto: Rise of a Ninja, transparente | DDS DXT5 |

As dimensões são do canvas do asset, não necessariamente da área visível do personagem. A arte de 452×624 perde nitidez se ampliada muito. O canvas de 1280×724 tem espaço transparente à direita e um contorno vermelho intencional.

### Complemento — logo original (2026-10-25)
Pedido adicional: verificar a presença do logo para uma composição com menu centralizado. O PNG `6e0003ce_I15.png` foi inspecionado e contém o logo completo, inclusive o subtítulo metálico, com fundo transparente. Ele pertence a Menu/PressStart. Chave de textura 00010B43, offset 38.839.724, 49.264 bytes comprimidos e DDS de 152.192 bytes, 528×288, DXT5. Uma nova leitura seletiva + zlib foi comparada byte a byte com o DDS já extraído; SHA-256 d4e4f3d8ae4e950a9f95038d2a03c490c7099c69dbbdf1028f51714314104202. Evidência: `out/prelaunch-art-discovery-2026-10-25/logo-findings.json`. O logo pode substituir os dois textos do título e ficar acima de uma coluna de menu centralizada; desenho e implementação dessa composição não foram realizados nesta verificação. A leitura do asset continua condicionada à cópia local do jogo; sem ISO disponível, manter identificação textual/fundo próprio.

## Leitura mínima demonstrada do SRA
- Cabeçalho: FRSA em 0, versão u32 LE em 4; offset/quantidade do índice em 8/12; offset/quantidade dos nomes em 16/20.
- Nesta cópia: índice em 69.012.500, nomes em 69.118.032.
- Registro do índice: 28 bytes / 7 u32 LE. Campos observados: tipo em +0, chave em +4, campo não interpretado em +8, offset de payload em +12, tamanho armazenado em +16, tamanho descomprimido em +20, flags em +24. Não atribuir significado não comprovado ao campo +8.
- Registro de nome: 140 bytes. Campo +0 foi 1 nos registros examinados; tipo em +4; chave em +8; nome ASCII terminado em zero nos 128 bytes a partir de +12. Cruzar (tipo, chave), não apenas chave.
- Naruto 6e0003ce_I13: chave de textura 000209C4, payload em 38.769.449, 70.275 bytes comprimidos; DDS de 282.176 bytes. SHA-256 do DDS: e386ad093eda982043a2a0e45a3495795f97bceda829bb9c6011280edc954b70.
- Não fixar esses offsets no produto: são evidência desta revisão, e o leitor deve consultar o índice, validar intervalos/tamanhos e aceitar falha com fallback.

## Integração possível, ainda não implementada
1. Após GameChoice concluir a validação como kReady, abrir Naruto_clip.SRA na ISO ou pasta escolhida.
2. Resolver o nome aprovado no diretório e ler apenas catálogo/payload necessários; impor limites de entrada, descompressão e dimensões.
3. Converter DDS BC3 para RGBA, ou usar uma rota suportada de upload BC3. O menu atual tem CreateImage/LoadTexture/GenerateTexture, mas não está conectado a esse arquivo.
4. Publicar a textura na thread apropriada, preservar alpha e liberar ao trocar jogo/fechar a tela. Confirmar convenção de alpha no runtime; GenerateTexture do RmlUi faz unpremultiply e não deve ser confundido com uma entrada RGBA arbitrária.
5. Enquanto carrega, ou em erro/ausência, manter o fundo abstrato. Não depender de iniciar o runtime guest.

Fontes: app/src/game_image.cpp:89 usa DiscImageDevice, ResolvePath e File::ReadSync; sdk/src/filesystem/devices/disc_image_file.cpp:28 aceita leitura por offset; app/src/menu/game_choice.cpp:81 valida assincronamente; app/src/menu/render_interface.cpp:117 cria imagem RGBA.

## Evidências locais e identidades
Pasta ignorada pelo Git: `out/prelaunch-art-discovery-2026-10-25/`.
- findings.json: hashes dos arquivos, offsets, nomes, dimensões e hashes dos DDS selecionados; releitura independente por seek + zlib comparada byte a byte.
- bf-index.tsv, clip-directory.json, selected-textures.json, decoded-art.json: inventários e resultados.
- PNGs nomeados pelos assets e saveicon.png: provas visuais locais, não inserir no release/repositório.
- Naruto_clip.SRA SHA-256: b6e9219301b8f5909cee6324b00f499915adb955a2deaf6238762c65fc16b72d.
- naruto.bf SHA-256: 537d5fba7677c8bd0fddcde8d142306c178cd5b5db803e78138112c236df6fdf.
- Naruto.SRA SHA-256: 8c398fdd8e72f37ded2f0007b508ba44f13ce840594a78170c2927a9b04ed8dd.

## Ferramentas, fontes e limites
- Sondas executadas em Python 3.14 com bibliotecas padrão (struct, mmap, zlib, hashlib); previews DDS decodificados para PNG com alpha e abertos com view_image. Sem instalar dependências.
- Estrutura externa BIG confrontada com sally_bf.bms, blob afb7dbb1c98f2d8ab1ab810592007ccc948f294d: https://github.com/RetingencyPlan/le_quickbms_script_compendium/blob/master/sally_bf.bms . Script apenas consultado, não incorporado ao port.
- A sonda de descompressão LZO foi confrontada com sdk/thirdparty/FFmpeg/libavutil/lzo.c (LGPL) para investigação; não incorporada ao produto e dispensável no caminho SRA selecionado.
- O protótipo histórico 8e51bd1 forneceu exemplos de corpos GFx para comparação. Não se confirmou a hipótese antiga de que meras ocorrências de CWS no BF fossem SWFs válidos; não usar essa busca como prova de localização.
- REA MCP ausente no catálogo desta sessão. O wrapper `node tools/native-renderer/rea.mjs --help` falhou por ENOENT de out/start-screen-study/2026-10-12-rea-upgrade2/installation.json. Nenhuma evidência REA foi produzida, nem a configuração foi alterada. A inspeção direta e os previews resolveram a pergunta.
- Testado com arquivos locais extraídos, não com uma integração executada dentro da ISO. Não houve build, boot do jogo, alteração de código ou alteração do FigCraft.
- Tempos em findings.json são de leitura/inflate com cache quente, sem DDS→RGBA, upload, ISO ou UI. Não são promessa de tempo do produto.
- Viabilidade técnica não é uma conclusão jurídica nem uma licença para distribuir assets.
