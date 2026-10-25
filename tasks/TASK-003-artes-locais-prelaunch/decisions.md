# Decisões — TASK-003

### DEC-001 — Preferir textura individual de Naruto_clip.SRA
- Estado: APPROVED
- Contexto: DDS BC3 legível fora do jogo, com nome no catálogo e transparência.
- Alternativas: ler textura SRA individual; executar GFx inteiro; renderizar modelo 3D; usar ícone BF de 64×64.
- Justificativa: acesso a imagem individual foi demonstrado, sem runtime guest ou execução de Flash; mantém o fallback abstrato antes da seleção da ISO.
- Impacto: REQ-001, REQ-002, REQ-003. Escolha estética final e implementação pendentes.
- Aprovação: usuário autorizou a tela e a troca pelo asset em 2026-10-25; asset escolhido é o logo 6e0003ce_I15, conforme F0 v7.

### DEC-002 — CPU assíncrona, GPU/UI na thread proprietária e fallback
- Estado: APPROVED no escopo do pedido de implementação de 2026-10-25.
- Justificativa: leitura e inflate não devem bloquear eventos; somente resultados da origem ainda ativa podem aparecer. Falha no logo não invalida uma ISO que já passou a identificação.
- Implementação: usar o stb já presente para inflate limitado, decoder BC3 isolado, textura dinâmica na interface existente e fundo procedural próprio. Remover capa/espiral embutidas da tela antiga. Sem alterações de SDK ou dependências novas.
- Impacto: REQ-005 a REQ-008; STEP-002/003.
