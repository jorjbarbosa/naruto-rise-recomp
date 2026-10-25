# Progresso — TASK-003

Estado da task: DONE
Aprovação de implementação: APROVADA pelo usuário
Origem e data da aprovação: sessão atual, 2026-10-25, após esboços F0 v7 e ajuste “DLC”: “pode implementar a tela e a troca com o asset”. Autoriza a tela e o carregamento do logo local descritos em STEP-002/003; não autoriza redistribuir assets.

| Etapa | Estado | Evidência | Próxima ação |
|---|---|---|---|
| STEP-001 | DONE | Catálogos, DDS, PNGs e findings.json | Concluída |
| STEP-002 | DONE | reader-final: parser inválido, zlib, BC3/alpha, pasta Unicode, ISO sintética/truncada e logo real em pasta/ISO; pixels iguais à investigação | Concluída |
| STEP-003 | DONE | Build Release; 859 checks GPU/UI por API, 8 boots do logo, 15 regressões de setup, sete idiomas; layout inspecionado | Entrega ao usuário |

## Limites e pendências
- Integração testada na ISO local real e na pasta extraída. Sem publicação, commit ou mudança de licença.
- REA indisponível por manifesto ausente; inspeção direta suficiente para esta pergunta.
- Artefatos extraídos permanecem somente na pasta local ignorada out/.
- Logo 6e0003ce_I15 (528×288, alpha) aplicado dinamicamente; a arte não é distribuída pelo produto. Dados de origem e mudanças locais anteriores preservados.
