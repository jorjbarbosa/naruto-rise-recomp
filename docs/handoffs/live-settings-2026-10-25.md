# Checkpoint — opções durante o jogo — 2026-10-25

Implementação entregue. Validação do executável concluída nas duas APIs. A investigação da violação de acesso da suíte de interface foi adiada expressamente pelo usuário para uma nova sessão. Não declarar a regressão completa aprovada.

## Contexto e autorização

- Pedido: opções aplicadas sem reiniciar, preservando reinício para API gráfica, API de entrada, adaptador e idioma do jogo. Aprovação: “pode implementar”, registrada em `out/live-settings/implementation/approval.json`.
- O usuário pediu trabalhar pelo código, sem recorrer ao plano. Esta entrega foi uma atividade ad hoc, fora de `tasks/`; nenhum TASK-ID foi criado e o plano F5 não foi consultado/atualizado.
- Última instrução: “finalize o que estiver faltando e em uma nova sessão nós investigamos isso, aqui está lento e demorando muito”. Não continuar investigando nesta sessão.
- Há trabalho simultâneo de outro agente na tela inicial e alterações locais anteriores. Não descartar mudanças nem atribuir toda a diff a esta entrega.

## Estado real

- Repositório: `C:/Users/Jorge/dev/naruto-rise-dev`.
- Branch: `feat/no-launcher`. Baseline da implementação: `4355dd2364cf0322113b25a158dac134841e25f8`. Código integrado no commit `44462fd508a4c528dc947c1c9acc34b681cbce65`. Conferir o HEAD atual ao retomar; não houve publicação.
- Implementação integrada deliberadamente a partir de `C:/Users/Jorge/dev/naruto-rise-live-settings-20261025`.
- Manifesto dos 27 arquivos da entrega: `out/live-settings/implementation/integration.json`. Baseline e backups: `baseline-files/` e `integration-before/`, no mesmo diretório.
- Compilação isolada: `C:/Users/Jorge/dev/naruto-rise-live-settings-20261025/app/out/build/live`.
- Produto pronto: `app/out/build/live-settings-product`. `Jogar.lnk` preserva os argumentos do atalho existente, incluindo o diretório atual de saves. O atalho da área de trabalho não foi alterado.
- SHA-256 do produto: `b90ce64defefcf2f48f3d8e9e8d0847369353898e7e8155d931534f1ac8ebdb7`.

## Implementação

- `gpu/settings.cpp/.h`: snapshots por valor protegidos por mutex; fila de revisão; aplicação no fim do quadro, atrás do trabalho anterior do dispatcher; publicação antes da próxima tradução.
- `gpu/backend/engine.*`, `backend.h`, `backend/reconfigure.h` e D3D12/Vulkan: reconfiguração no owner após GPU idle, criação de alvos candidatos, restauração após falha, preservação de texturas resolvidas e alteração de apresentação. O MSAA usado pela pré-compilação paralela tem publicação atômica.
- `gpu/d3d/frame.cpp`: consumo da fila no limite entre quadros.
- `gpu/interpolation/tick.cpp` e `transforms.*`: reset do relógio/histórico no começo da iteração completa (MapStep, depois WorldChangeStep e MainStep), sem alterar o passo da lógica do jogo. Ordem confirmada no código gerado em 82684A14..82684A20.
- `gpu/textures/textures.cpp`: invalidação dos dois caches de samplers quando o filtro muda.
- `menu/settings.*`, `panel.*`, `ui.h`, `narutorise_app.h`, `text.cpp`: confirmação assíncrona, valor aplicado após aceite, restauração após recusa e preservação do foco. Removida indicação de reinício das opções implementadas.
- `intro_skip.*`: política atômica para futuras aberturas de vídeos.
- Opções: resolução, brilho, FPS, VSync, MSAA, FXAA, CAS/FSR, filtro anisotrópico, AA de transparência, latência, buffers e futuros vídeos de introdução.
- Vulkan mantém latência efetiva 1 e respeita os limites de imagens da superfície. Troca dos arquivos do jogo também mantém reinício.

## Evidência executada

- `product-d3d12-ready/results.json`: 34/34, produto final, encerramento normal.
- `product-vulkan-ready/results.json`: 33/33, produto final no save da vila, encerramento normal.
- `final-gameplay-d3d12/results.json`: 34/34, gameplay na vila em compilação anterior à inclusão do cache opcional de pipelines.
- `verified-vulkan-ack/results.json`: 32/32, produto final na tela do jogo.
- Seis testes de GPU passaram: reconfigure, present_edge e depth_resolve em D3D12 e Vulkan. Evidência em `final-gpu/`. O teste reconfigure verifica pixels, dimensões, MSAA, retenção de texturas, apresentação e continuidade após rejeitar configuração inválida.
- Suítes de interface anteriores passaram nas duas APIs (`menu-*` e `final-menu-*`). A versão mais recente integra também um teste novo de nomes de DLC; sua queda não foi diagnosticada e não pode ser atribuída a esse teste sem evidência.
- A automação inicial teve problemas de foco/espera/navegação; ficaram preservados. O harness atual aguarda a aba realmente renderizada e, com fixture, o HUD da vila, em vez de confiar em espera fixa.
- `git diff --check -- app/src app/CMakeLists.txt app/tests/live_settings_boot_test.py` passou. A checagem da branch inteira também detectou uma linha em branco já existente em `.gitignore`; não foi alterada.

## Violação de acesso — pendência para a próxima sessão

- Executável: `native_engine_menu_test.exe`, D3D12.
- SHA-256: `a35b6d8b91afa102607ae554360f0e2ad883d9a33162a0457589d72e10304b8d`.
- Saída: `-1073741819 / 0xC0000005 / STATUS_ACCESS_VIOLATION`.
- 855 asserções registradas; nenhuma com `ok=false`; queda antes do verdict.
- Último registro: `notification-release-action`, ao fim do exercício dos menus, antes de concluir a suíte.
- Endereço, tipo do acesso e pilha ainda não capturados. Causa desconhecida.
- Registro mínimo versionado: [`docs/qa/menu-access-violation-2026-10-25.json`](../qa/menu-access-violation-2026-10-25.json).
- Log completo local: `out/live-settings/implementation/access-violation/violacao-de-acesso.log`.
- Evidência e JSONL completos: `evidence.json` e `menu-results.jsonl` no mesmo diretório.
- A tentativa de iniciar LLDB do Visual Studio falhou com `0xC0000135 / STATUS_DLL_NOT_FOUND`. É uma falha separada do debugger; não produziu pilha.
- A última suíte Vulkan e a rodada extra com validation layer não foram executadas: o comando agrupado parou após a queda de D3D12.

## Próximo passo exato

1. Ler este checkpoint e a evidência da violação; conferir Git e hashes atuais.
2. Capturar a exceção com debugger funcional ou dump, investigando o encerramento da suíte. Não presumir a causa pelo último teste registrado.
3. Reproduzir com saída nova; o teste recusa arquivos de resultado já existentes:

```powershell
$env:REX_NATIVE_TEST_API = 'd3d12'
$reproDir = Join-Path $env:TEMP ('narutorise-menu-repro-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $reproDir | Out-Null
& 'C:/Users/Jorge/dev/naruto-rise-live-settings-20261025/app/out/build/live/native_engine_menu_test.exe' (Join-Path $reproDir 'results.jsonl')
$LASTEXITCODE
```

4. Corrigida a causa, concluir as duas suítes de interface; repetir testes do produto apenas se alterações novas justificarem.
5. `out/live-settings/implementation/finalize.py` não foi executado: seu gate exige a regressão completa. O resumo atual em `summary.json` registra explicitamente a pendência.

Para reconstruir somente o teste, usar o script isolado, com um log novo:

```powershell
& out/live-settings/implementation/build.ps1 -Log "$PWD/out/live-settings/implementation/menu-next-session-build.log" -Target @('native_engine_menu_test')
```


## Faxina e commits — 2026-10-25

- O usuário autorizou organizar e commitar o trabalho pendente na branch. Código e documentação do produto foram integrados em `44462fd`; formulários de issues, ferramenta de corpus e regras locais em `3cdf5b6`.
- A documentação agora descreve corretamente as opções ao vivo. Não houve alteração de licença nem investigação adicional da violação de acesso.
- Regressões menores reexecutadas nos binários existentes: configuração, teclas, localização, idiomas do jogo, verificação de atualização sem rede, DLC e leitor do logo: sete saídas 0. Evidência local: `out/branch-cleanup-checks-20261025-154402/results.json`.
- Validação de três tasks, sintaxe de dez scripts Python, três scripts PowerShell, JSON de QA, gate de empacotamento com nativo OFF e extração de corpus com dados sintéticos passaram. Parser YAML indisponível; formulários revisados como texto.
- Quatro arquivos experimentais sem ligação no build foram preservados em `out/branch-cleanup-20261025-154039/`, com manifesto de caminhos e SHA-256: step_split.cpp/.h e os patches palette-thread-probe/step-split-wiring. Nenhuma sonda foi incorporada ao produto.
- `sdk/` permanece no próprio repositório Git, em HEAD destacado, com alterações locais anteriores. Não foi alterado, commitado nem incluído como novo gitlink nesta faxina.
- Builds, dados do jogo, caches, logs completos e artefatos locais continuam fora do Git. O registro mínimo da falha acompanha este checkpoint no repositório.
