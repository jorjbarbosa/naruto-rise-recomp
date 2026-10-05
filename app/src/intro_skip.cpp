// narutorise - ReXGlue Recompiled Project
//
// Skip opcional dos vídeos de abertura (configurável pela cvar/toml).
//
// A engine Jade abre vídeos .bik como arquivos comuns. Este hook intercepta o
// wrapper de abertura de arquivo do motor (sub_822DCA80): com
// `skip_intro_videos` ativo, aberturas dos vídeos de boot percorrem o caminho
// de falha que o wrapper traduz para o código 80 (log do motor + handle -1), e
// o player de vídeo pula o clipe sem tratamento especial.

#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/narutorise_init.h"

REXCVAR_DEFINE_BOOL(skip_intro_videos, false, "narutorise",
                    "Skip the boot intro videos (Ubisoft and logo videos)");

namespace {

// Funções geradas usadas pelo caminho de falha do wrapper (ver o corpo gerado
// de sub_822DCA80 em narutorise_recomp.88.cpp).
extern "C" void __imp__sub_822DCA80(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8217F570(PPCContext& __restrict, uint8_t*);  // log de falha (NTSTATUS)
extern "C" void __imp__sub_8217F550(PPCContext& __restrict, uint8_t*);  // log de resultado (código)

// Apesar de 0xC0000035 ser STATUS_OBJECT_NAME_COLLISION, o wrapper original
// sub_822DCA80 trata especificamente esse status como código 80. Preservamos
// essa tradução peculiar do motor Jade para acionar o caminho de skip.
constexpr uint32_t kEngineSkipStatus = 0xC0000035;
constexpr uint32_t kEngineSkipError = 80;
constexpr uint32_t kInvalidGuestHandle = 0xFFFFFFFF;

// Lê uma C string ASCII da memória guest (bytes não precisam de bswap).
std::string ReadGuestPath(uint8_t* base, uint32_t addr) {
  std::string out;
  if (!addr) return out;
  for (size_t i = 0; i < 260; ++i) {
    char c = static_cast<char>(REX_LOAD_U8(addr + i));
    if (!c) break;
    out.push_back(c);
  }
  return out;
}

std::string ToLower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c += 32;
  }
  return s;
}

bool IsIntroVideo(const std::string& lower_path) {
  // Compara o NOME exato do arquivo: substrings como "_intro_" aparecem em
  // cutscenes de história (ex.: 110_Intro_Haku_Combat.bik, 301_Jiraya_Intro.bik)
  // e não devem ser puladas.
  auto sep = lower_path.find_last_of("\\/");
  const std::string name =
      sep == std::string::npos ? lower_path : lower_path.substr(sep + 1);
  return name == "ubisoft.bik" || name == "intro.bik" ||
         name == "logo_corpo_emea.bik" || name == "logo_corpo_us.bik";
}

}  // namespace

REX_HOOK_RAW(sub_822DCA80) {
  if (REXCVAR_GET(skip_intro_videos)) {
    std::string path = ToLower(ReadGuestPath(base, ctx.r3.u32));
    if (IsIntroVideo(path)) {
      REXLOG_INFO("[narutorise] intro video skipped: {}", path);
      // Espelha o caminho de falha especial do wrapper original:
      ctx.r3.u64 = kEngineSkipStatus;
      __imp__sub_8217F570(ctx, base);  // log de falha da engine
      ctx.r3.u64 = kEngineSkipError;
      __imp__sub_8217F550(ctx, base);  // log de resultado da engine
      ctx.r3.u64 = kInvalidGuestHandle;
      return;
    }
  }
  __imp__sub_822DCA80(ctx, base);
}
