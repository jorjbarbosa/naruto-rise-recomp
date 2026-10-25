# Testes — TASK-003

## Implementação autorizada em 2026-10-25

### TEST-004 — Parser FRSA/zlib/DDS limitado
- Requisitos: REQ-005, REQ-008
- Etapas: STEP-002
- Tipo: teste C++ com fixtures sintéticas
- Procedimento planejado: construir e executar narutorise_game_logo_test; sucesso BC3/alpha, nome ausente, tabelas/tamanhos/versão inválidos, truncamento, checksum zlib e DDS não suportado.
- Resultado: PASS
- Evidência: build narutorise_game_logo_test e execução final `app/out/build/win-amd64-release/narutorise_game_logo_test.exe out/prelaunch-implementation-2026-10-25/reader-final game C:/Users/Jorge/Naruto-Rise-of-a-ninja.iso`. Casos sintéticos, BC3/alpha e ISO truncada passaram. Primeira linkagem expôs macro STBI_SUPPORT_ZLIB ausente, corrigida antes do PASS.

### TEST-005 — Pasta e ISO com leitura seletiva
- Requisitos: REQ-005, REQ-006
- Etapas: STEP-002
- Tipo: integração C++ com pasta/ISO sintéticos e cópia local real
- Procedimento planejado: mesma imagem via pasta/ISO/XEX; asset real 528×288; contagem de bytes lidos e fallback em arquivo ausente.
- Resultado: PASS
- Evidência: execução reader-final, pasta Unicode e ISO sintética; logo real em pasta e ISO comercial 528×288. Comparação independente byte a byte entre pasta, ISO e RGBA do PNG da investigação, registrada em reader-final/pixel-verification.json.

### TEST-006 — UI, foco, alpha e resultado assíncrono
- Requisitos: REQ-006, REQ-007
- Etapas: STEP-003
- Tipo: regressão GPU/UI
- Procedimento planejado: atualizar expectativas do layout no native_engine_menu_test, testar caminho sem logo e ciclo de troca/fechamento, navegação e idiomas; executar D3D12/Vulkan conforme infraestrutura existente.
- Resultado: PASS
- Evidência: native_engine_menu_test com NARUTORISE_TEST_GAME_ROOT=C:/Users/Jorge/dev/naruto-rise-dev/game; D3D12 em menu-d3d12-02.jsonl e Vulkan (REX_NATIVE_TEST_API=vulkan) em menu-vulkan-final.jsonl. 859 checks por API, zero falhas; foco, mouse, controle, alpha, logo, troca de origem, resultado após Hide/Show e handover. Capturas de boot inspecionadas.

### TEST-007 — Boots isolados com ISO e fallback
- Requisitos: REQ-006, REQ-007
- Etapas: STEP-003
- Tipo: integração Windows
- Procedimento planejado: produto em diretório isolado, primeiras telas sem jogo e --setup com ISO, capturas, logo e fechamento; ambas as APIs nativas; preservar dados pessoais.
- Resultado: PASS
- Evidência: `python -I app/tests/prelaunch_logo_boot_test.py app/out/build/win-amd64-release game out/prelaunch-implementation-2026-10-25/boot-final C:/Users/Jorge/Naruto-Rise-of-a-ninja.iso`: 8 casos, zero falhas, duas APIs, arquivo ausente/corrompido, seleção nativa da ISO real com troca ao vivo. As tentativas anteriores de automação do seletor falharam por controles expostos como Pane/read-only; o teste passou com controle Win32 e fallback de teclado restrito ao HWND do diálogo, sem mudar o código do seletor do produto.

### TEST-008 — Ausência de arte embutida e regressões
- Requisitos: REQ-004, REQ-008
- Etapas: STEP-003
- Tipo: inspeção/build/testes existentes
- Procedimento planejado: conferir Asset() e embeds, nenhum novo PNG proprietário em app/ui; build do produto e regressões game_image/game_choice/setup_gate pertinentes; diff limitado sem descartar mudanças anteriores.
- Resultado: PASS
- Evidência: build Release narutorise/native_engine_menu_test e alvos de regressão. game_image (20 checks), setup_gate (205 checks), game_choice e localization passaram. `setup_boot_test.py app/out/build/win-amd64-release C:/Users/Jorge/Naruto-Rise-of-a-ninja.iso out/prelaunch-implementation-2026-10-25/setup-regression`: 15 casos, sete CRCs de idioma distintos, handover nas duas APIs, CLI e caminho Unicode. py_compile dos dois scripts passou. Verificação Asset() em teste GPU e busca dos embeds confirmam ausência de capa/espiral/logo distribuídos pela UI. git diff --check dos arquivos do escopo sem erros (somente avisos LF/CRLF).
- Validação documental: scripts/validate_tasks.py identificou dois campos Resultado com qualificadores em TASK-002, criada nesta mesma sessão; foram normalizados para PASS, movendo os qualificadores a Limite, sem alterar a conclusão da avaliação de licença.

### TEST-001 — Inventário e identidades
- Requisitos: REQ-001, REQ-004
- Etapas: STEP-001
- Tipo: inspeção binária offline
- Procedimento executado: leitura mmap/struct dos cabeçalhos BIG e FRSA, registros e nomes; SHA-256 dos três arquivos de entrada.
- Resultado: PASS
- Evidência: bf-index.tsv, clip-directory.json, findings.json; 812 entradas BF, 3.769 registros e 3.756 nomes em Naruto_clip.SRA.
- Data/ambiente: 2026-10-25, Python 3.14/Windows, HEAD 4355dd2 com alterações locais preexistentes.

### TEST-002 — Extração seletiva e prova visual
- Requisitos: REQ-001, REQ-002
- Etapas: STEP-001
- Tipo: experimento de leitura/decodificação
- Procedimento executado: consulta por (tipo,chave); seek/read de payload; zlib com conferência do tamanho; identificação DDS; decodificação DXT1/DXT5 para RGBA; gravação PNG; inspeção via view_image; segunda leitura seletiva comparada byte a byte com o DDS inicial.
- Resultado: PASS
- Limite: oito candidatos de findings.json; dez previews gerados em decoded-art.json, dos quais os oito candidatos da tabela foram visualmente conferidos.
- Evidência: Naruto em pose de chute 452×624, Naruto apontando 1280×724, retrato 352×432, céu, nuvens e efeitos listados em context.md. DDS/PNGs sob out/prelaunch-art-discovery-2026-10-25/.
- Data/ambiente: 2026-10-25; arquivos extraídos locais, sem executar o guest.

Complemento de TEST-002 na mesma data: `6e0003ce_I15`, logo completo 528×288, visualmente confirmado; nova leitura do SRA, zlib, validação de tamanho, comparação byte a byte com o DDS anterior e SHA-256 registrados em logo-findings.json. Resultado PASS para esse asset adicional. Sem teste runtime ou mudança do FigCraft.

### TEST-003 — Viabilidade de integração e preservação
- Requisitos: REQ-003, REQ-004
- Etapas: STEP-001
- Tipo: leitura de código e verificação documental
- Procedimento executado: leitura de game_image.cpp, game_choice.cpp, render_interface.cpp e assinaturas DiscImageFile::ReadSync; git status e git check-ignore para a pasta de evidências.
- Resultado: PASS
- Limite histórico: existência dos pontos de extensão; runtime ainda não executado nessa fase. A execução posterior está em TEST-006/007/008.
- Evidência: ISO oferece leitura por offset e menu aceita RGBA; novo leitor SRA/decodificador ainda não conectado. Pasta de evidência ignorada pelo Git, arquivos de origem somente lidos.
- Data/ambiente: 2026-10-25; nenhum build, teste de jogo ou mudança da UI executados.
