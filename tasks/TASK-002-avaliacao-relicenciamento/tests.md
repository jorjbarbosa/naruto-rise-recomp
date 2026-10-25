# Testes — TASK-002

As verificações abaixo são documentais. Nenhum teste de aplicação, build ou execução de jogo foi realizado.

### TEST-001 — Histórico de licença e autoria registrada
- Requisitos: REQ-001
- Etapas: STEP-001
- Tipo: inspeção documental
- Procedimento/comando executado: git status --short; git shortlog -sn HEAD; git log --follow -- LICENSE; git show 54657ac; git show 277056e:LICENSE; git rev-parse --is-shallow-repository.
- Resultado: PASS
- Evidência: GPLv3 atual; troca MIT→GPL em 18/10/2026; 108 commits de Jorge Barbosa em HEAD; histórico não superficial.
- Data/ambiente: 2026-10-25, Windows/PowerShell, feat/no-launcher @ 4355dd2.

### TEST-002 — Dependências e FFmpeg
- Requisitos: REQ-002, REQ-003
- Etapas: STEP-001
- Tipo: inspeção documental
- Procedimento/comando executado: leitura de sdk/LICENSE, tools/xenosrecomp/LICENSE.md, manifesto de packaging, FreeType/LICENSE.TXT, RmlUi/LICENSE.txt, Plume notice; rg dos CONFIG_GPL/CONFIG_NONFREE/CONFIG_VERSION3 em config_windows_x86_64.h; leitura de sdk/thirdparty/CMakeLists.txt e package-release.ps1.
- Resultado: PASS
- Limite: escopo documental; binários não auditados.
- Evidência: dependências principais permissivas; FFmpeg configurado como LGPL sem componentes GPL; FFmpeg/libmspack estáticos em rexruntime.dll exigem preservar cumprimento LGPL.
- Data/ambiente: 2026-10-25, árvore de trabalho local.

### TEST-003 — Procedência, limites e preservação
- Requisitos: REQ-003, REQ-004
- Etapas: STEP-001
- Tipo: inspeção documental
- Procedimento/comando executado: busca de avisos GPL em app/src, app/ui, tools/native-renderer e patches/sdk; inspeção de referências ao Unleashed; leitura da declaração do usuário e registro das fontes oficiais.
- Resultado: PASS
- Limite: registro documental; titularidade jurídica não certificada.
- Evidência: busca não encontrou cabeçalhos GPL nos arquivos de código pesquisados, o que não prova ausência de derivações; usuário declarou não copiar código. Escritas desta investigação limitadas aos sete documentos da task.
- Data/ambiente: 2026-10-25, mesma sessão.
