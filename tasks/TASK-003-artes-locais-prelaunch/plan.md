# Plano — TASK-003

Status: DONE. Escopo F0 v7 e troca pelo logo local autorizados pelo usuário em 2026-10-25; STEP-001/002/003 concluídas, com evidências em tests.md.

### STEP-001 — Inventariar e comprovar artes locais
- Objetivo: responder à viabilidade com conteúdo real.
- Requisitos: REQ-001, REQ-002, REQ-003, REQ-004
- Testes: TEST-001, TEST-002, TEST-003
- Arquivos previstos: documentos desta task; out/prelaunch-art-discovery-2026-10-25 para evidência local ignorada.
- Dependências: cópia local já disponível, ferramentas de inspeção existentes.
- Mudanças: nenhum código do produto; extração/decodificação analítica e documentação.
- Critério de pronto: assets vistos, acesso individual validado e integração proposta com limites.
- Riscos: layout específico da revisão, alpha, arquivos incompletos e risco de tratar hipótese como runtime comprovado.

### STEP-002 — Leitor e decodificador do logo local
- Objetivo: obter RGBA de 6e0003ce_I15 em pasta/ISO sem extrair o jogo.
- Requisitos: REQ-004, REQ-005, REQ-008
- Testes: TEST-004, TEST-005
- Arquivos previstos: app/src/menu/game_logo.h/.cpp, app/tests/game_logo_test.cpp, app/CMakeLists.txt.
- Dependências: STEP-001 DONE.
- Mudanças: leitura por callback/offset, lookup FRSA, inflate limitado usando stb já disponível (seu aviso já é distribuído), verificação zlib e DDS BC3; adaptadores pasta/ISO; fixtures sintéticas sem assets proprietários.
- Critério de pronto: testes de parser e arquivos locais/ISO passam, sem offsets fixos nem novos pacotes.
- Riscos: dados truncados, overflow, descompressão excessiva, convenção alpha.

### STEP-003 — Tela centralizada e troca assíncrona
- Objetivo: reproduzir o F0 v7, preservando ações e estados existentes.
- Requisitos: REQ-004, REQ-006, REQ-007, REQ-008
- Testes: TEST-006, TEST-007, TEST-008
- Arquivos previstos: app/src/menu/setup_screen.*, render_interface.*, ui.cpp, assets.cpp, text.cpp; app/src/narutorise_app.h; app/ui/setup.rml/.rcss; testes de UI/boot e docs relacionados.
- Dependências: STEP-002 DONE.
- Mudanças: State recebe origem validada, worker CPU do logo com descarte de resultado obsoleto; textura dinâmica na UI; fundo próprio procedural e layout centralizado; remover embeds da capa/espiral; manter fallback, sete idiomas, controles e fechamento.
- Critério de pronto: build e regressões relevantes passam, inspeção visual sem jogo/com logo e validação de fallback/ISO em ambas as APIs nativas.
- Riscos: cache RmlUi, alpha, callbacks durante Refresh, fechamento com leitura pendente, testes antigos baseados no layout anterior.
