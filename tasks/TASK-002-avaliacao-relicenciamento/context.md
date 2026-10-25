# Contexto — TASK-002

## Objetivo
Avaliar a possibilidade de substituir a GPL do código próprio do port. Pedido em 2026-10-25: verificar se é possível remover a GPL sem problemas. Não foi solicitada nem aprovada uma alteração de licença.

## Arquitetura e comportamento confirmado
- Branch `feat/no-launcher`, HEAD `4355dd2364cf0322113b25a158dac134841e25f8`; histórico local não superficial. `git shortlog -sn HEAD` mostrou 108 commits de Jorge Barbosa. Isso identifica autoria registrada, não comprova titularidade jurídica.
- `LICENSE` contém GPLv3. `README.md:150` declara GPL-3.0-only para o código próprio e registra a publicação de v0.2.0-beta.1 sob MIT.
- `git show 277056e:LICENSE` contém MIT com copyright de Jorge Barbosa. O commit `54657ac3a12da6007d5ae8ad76a68f1a4f0b97b2`, de 2026-10-18, substituiu MIT por GPL-3.0-only em LICENSE, README e empacotamento. A mensagem não registra uma imposição de dependência.
- Usuário declarou nesta sessão: **“eu não copiei código de outros projetos”**. A conclusão sobre o código próprio considera essa declaração. Dependências explicitamente incorporadas permanecem de terceiros.
- ReXGlue: BSD-3-Clause (`sdk/LICENSE`), incluindo atribuições ao Xenia. XenosRecomp: MIT (`tools/xenosrecomp/LICENSE.md`). Plume e alocadores: licenças permissivas registradas no manifesto de distribuição. RmlUi: MIT; FreeType: opção FTL explicitamente escolhida; fontes Roboto: OFL-1.1.
- FFmpeg Windows x64: `sdk/thirdparty/FFmpeg/config.h` seleciona `config_windows_x86_64.h`, que declara LGPL 2.1 ou posterior, `CONFIG_GPL 0`, `CONFIG_NONFREE 0`, `CONFIG_VERSION3 0`, `CONFIG_GPLV3 0`.
- `sdk/thirdparty/CMakeLists.txt:130`, `:452` e `:569` criam mspack, libavutil e libavcodec como bibliotecas estáticas. `packaging/package-release.ps1:122` e `:174` registram sua incorporação ao rexruntime.dll e instruções para reconstruir/substituir essa DLL.
- `packaging/third-party-licenses.psd1` conserva o inventário separado das dependências; `packaging/package-release.ps1:166` escreve GPL-3.0-only para o código próprio no pacote.
- Comentários em `app/src/graphics_device.h` e `app/src/gpu/backend/pipeline_set.h`, além das notas locais de investigação, citam comportamento e arquitetura do UnleashedRecomp. A mera referência não prova cópia; não foi feita comparação integral de similaridade entre repositórios. A declaração do usuário esclarece a procedência afirmada.

## Conclusão e limites
Não foi identificado, no escopo examinado, componente que imponha GPL ao código próprio. Sendo Jorge titular dos direitos necessários sobre esse código, pode oferecê-lo sob outra licença, inclusive após publicação GPL. A nova licença não revoga as permissões das cópias anteriores distribuídas em conformidade com GPL/MIT.

Isso não autoriza remover licenças ou atribuições de dependências. LGPL continua exigindo cumprimento de suas condições, inclusive fontes correspondentes e meios de modificar/reconstruir/relinkar os componentes abrangidos. Fechar o código exige avaliar esses meios no pacote concreto; trocar para outra licença aberta é uma questão diferente de retirar avisos de terceiros.

A avaliação é técnica/documental, não garantia jurídica, auditoria integral de todos os binários ou prova de titularidade. Código recompilado, dados, shaders e elementos da franquia têm direitos independentes da licença do port.

## Dependências e restrições
- Existem extensas modificações e arquivos não rastreados anteriores em app, SDK, launcher, packaging, docs e tasks. Foram preservados. README e scripts de empacotamento examinados incluem alterações locais.
- Não foi compilado, executado ou empacotado o aplicativo; não há novos resultados de testes funcionais.
- Nenhuma ferramenta REA/IDA é necessária para esta questão documental.

## Fontes oficiais consultadas em 2026-10-25
- GNU FAQ, titular pode oferecer o mesmo código sob licenças diferentes: https://www.gnu.org/licenses/gpl-faq.en.html#ReleaseUnderGPLAndNF
- GPLv3, seção 2 (irrevogabilidade condicionada ao cumprimento): https://www.gnu.org/licenses/gpl.en.html ; texto também conferido em LICENSE:157.
- LGPLv2.1, seções 5 e 6: https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html ; texto também conferido em sdk/thirdparty/libmspack/libmspack/COPYING.LIB.
- FFmpeg: https://ffmpeg.org/legal.html ; configuração e licença da cópia local também examinadas.
- A busca web retornou os trechos oficiais; tentativas de abertura direta desses sites retornaram timeout. Não se presume consulta integral online onde ela falhou.

## Hipóteses não confirmadas
- Ausência de cessão de direitos a empregador/contratante ou outros contratos que restrinjam a titularidade.
- Correspondência completa entre todos os binários já publicados e suas fontes, patches e avisos.
- Licença de destino ainda não escolhida. Não se presume MIT nem fechamento do código.
