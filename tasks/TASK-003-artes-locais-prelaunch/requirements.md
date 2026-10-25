# Requisitos — TASK-003

## Escopo
Localizar artes reais e implementar o pré-lançamento centralizado aprovado no FigCraft (F0 v7), com logo original carregado da cópia local do jogo após validação. Ampliação autorizada em 2026-10-25: “pode implementar a tela e a troca com o asset”.

## Fora de escopo
Alterar ISO/assets de origem, publicar ou embutir arte do jogo, modificar licenças, refazer menus de configurações/DLC, ou declarar autorização jurídica.

- **REQ-001** — Identificar contêiner e assets. Critério: registrar arquivo, identidade, nome/chave e resolução com conteúdo visual confirmado.
- **REQ-002** — Demonstrar acesso offline. Critério: leitura de payload selecionado, descompressão íntegra e preview legível sem executar o jogo.
- **REQ-003** — Avaliar integração. Critério: indicar pontos existentes de leitura ISO e imagem UI, trabalho faltante e limites reais.
- **REQ-004** — Preservar arquivos do usuário e alterações anteriores. Critério: durante investigação, somente docs/evidências; após autorização, mudanças limitadas à tela, leitor, testes e integração necessários, sem editar dados de origem ou descartar trabalho local.
- **REQ-005** — Ler o logo pelo catálogo SRA com limites. Critério: lookup por nome/tipo/chave, validação de intervalos/tamanhos/versão, zlib e DDS BC3; falhas retornam ausência sem impedir o jogo; testes sintéticos incluem entradas truncadas e abusivas.
- **REQ-006** — Carregar de pasta e ISO sem bloquear o frame. Critério: trabalho CPU em worker sem acesso à UI/GPU; resultado obsoleto descartado ao trocar caminho/fechar; upload e liberação na thread UI; fallback textual em ausência/falha.
- **REQ-007** — Reproduzir os dois estados do F0 v7. Critério: título textual sem jogo; logo centralizado com jogo; menu centralizado com seleção laranja, DLC (quando pronto), sem kunai/capa/PORT PARA PC; footer com comandos localizados e versão; preserve teclado/mouse/controle e erros/modo seguro.
- **REQ-008** — Não distribuir o logo ou a capa anterior no produto. Critério: logo só vem de Naruto_clip.SRA local, nenhuma inclusão do PNG extraído ou fonte embutida; retirar embed da capa/espiral anteriores da UI.
