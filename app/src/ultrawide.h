#pragma once

#include <cstdint>

// Aplica o patch de projeção anamórfica do motor Jade.
// Não faz nada quando ultrawide_target_aspect <= 0.
void ApplyUltrawidePatch(uint8_t* base);
