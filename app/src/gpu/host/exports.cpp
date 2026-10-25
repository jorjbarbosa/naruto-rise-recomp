// The guard of the guest kernel exports in the native mode: VdInitializeEngines
// and VdSetGraphicsInterruptCallback on the own owners, the exports that the
// D3D boundary admits, the user-module procedure lookup, the capped wait
// records; any other graphics export stops.
#include "gpu/host/app.h"
#include "gpu/host/internal.h"
#include "gpu/d3d/d3d.h"
#include <bit>
#include <fmt/format.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/export_resolver.h>
#include <rex/system/kernel_module.h>
#include <rex/system/user_module.h>
#include <rex/system/xmemory.h>

namespace gpu::host {
namespace {
bool IsWait(std::string_view name) {
  return name == "__imp__KeWaitForSingleObject" || name == "__imp__KeWaitForMultipleObjects" ||
         name == "__imp__KeDelayExecutionThread" || name == "__imp__NtWaitForSingleObjectEx" ||
         name == "__imp__NtWaitForMultipleObjectsEx" ||
         name == "__imp__NtSignalAndWaitForSingleObjectEx";
}
// XexGetProcedureAddress by ordinal into a user module of the title
// (its executable or one of its own XEX DLLs, e.g. AI2C) returns guest code of
// that module, not an unguarded runtime export. Only that lookup is admitted, in
// the native mode: the export target must lie in the module code, and the result
// is the target or a dispatcher thunk (AllocateThunk, for the caller's module)
// that resolves to the same function. Kernel modules keep the guard.
struct UserProcedureLookup {
  bool active = false;
  uint32_t hmodule = 0, ordinal = 0, out = 0, lr = 0, target = 0;
  rex::runtime::XexModule* xex = nullptr;
  std::string module;
};
thread_local UserProcedureLookup user_lookup;
std::atomic<uint64_t> user_lookups{0};
bool AdmitUserProcedureLookup(PPCContext& ctx) {
  auto* kernel = Kernel();
  if (!gpu::Active() || !kernel || !ctx.r4.u32 || (ctx.r4.u32 & 0xFFFF0000u) || !ctx.r5.u32 ||
      user_lookup.active) return false;
  rex::system::object_ref<rex::system::XModule> module;
  if (!ctx.r3.u32) module = kernel->GetExecutableModule();
  else module = rex::system::XModule::GetFromHModule(kernel, kernel->memory()->TranslateVirtual(ctx.r3.u32));
  if (!module || module->module_type() != rex::system::XModule::ModuleType::kUserModule) return false;
  auto* user = static_cast<rex::system::UserModule*>(module.get());
  auto* xex = user->xex_module();
  const uint32_t target = xex ? xex->GetProcAddress(uint16_t(ctx.r4.u32)) : 0;
  if (!target || !xex->ContainsAddress(target)) return false;
  user_lookup = {true, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr), target, xex, user->name()};
  return true;
}
void CheckUserProcedureLookup(PPCContext& ctx) {
  if (!user_lookup.active) Stop("XexGetProcedureAddress-after-unadmitted", &ctx, 0, 0, 88);
  auto* ts = rex::runtime::ThreadState::Get();
  uint32_t raw = 0;
  const bool read = Read(ts->memory(), user_lookup.out, &raw, 4);
  const uint32_t status = ctx.r3.u32, pointer = std::byteswap(raw);
  auto* dispatcher = Kernel()->function_dispatcher();
  auto* function = dispatcher ? dispatcher->GetFunction(user_lookup.target) : nullptr;
  const bool thunk = pointer != user_lookup.target;
  const bool inside = read && status == 0 && pointer &&
      (!thunk || (function && dispatcher->GetFunction(pointer) == function));
  const auto lookup = user_lookup;
  user_lookup = {};
  if (user_lookups.fetch_add(1) < 16 || !inside) {
    Record("native-user-procedure-lookup", fmt::format(
        "\"module\":\"{}\",\"hmodule\":{},\"ordinal\":{},\"lr\":{},\"guest_thread\":{},\"status\":{},\"pointer\":{},"
        "\"target\":{},\"dispatcher_thunk\":{},\"same_function\":{},\"runtime_export_thunk\":false",
        lookup.module, lookup.hmodule, lookup.ordinal, lookup.lr, ts->thread_id(), status, pointer, lookup.target,
        thunk, inside));
  }
  if (!inside) Stop("XexGetProcedureAddress-result-not-the-export", &ctx, status, pointer, 88);
}
}  // namespace
bool BeforeExport(const char* name, PPCContext& ctx) {
  if (!enabled.load()) return false;
  if (std::string_view(name) == "__imp__VdInitializeEngines") return InitializeEngines(ctx);
  if (std::string_view(name) == "__imp__VdSetGraphicsInterruptCallback") {
    ValidateInterruptRegistration(ctx);
    std::lock_guard lock(event_mutex);
    Record("native-interrupt-register-before", name, &ctx);
    return false;  // Original void SDK service calls our real owned registration.
  }
  if (gpu::AdmitExport(name, ctx)) return false;
  if (std::string_view(name).starts_with("__imp__Vd")) Stop(name, &ctx);
  // A dynamic lookup could expose unguarded exports in rexruntime.dll's own
  // registry. Diagnose the lookup before it creates or returns any thunk.
  if (std::string_view(name) == "__imp__XexGetProcedureAddress") {
    if (!AdmitUserProcedureLookup(ctx)) Stop(name, &ctx);
    return false;  // Original SDK lookup inside a module of the title.
  }
  if (IsWait(name) && KeepRecord(wait_records)) {
    std::lock_guard lock(event_mutex);
    Record("wait-before-original-sdk", name, &ctx);
  }
  return false;
}
void AfterExport(const char* name, PPCContext& ctx) {
  if (!enabled.load()) return;
  gpu::ExportAfter(name, ctx);
  if (std::string_view(name) == "__imp__XexGetProcedureAddress") {
    CheckUserProcedureLookup(ctx);
    return;
  }
  if (std::string_view(name) == "__imp__VdSetGraphicsInterruptCallback") {
    std::lock_guard lock(event_mutex);
    Record("native-interrupt-register-after", name, &ctx);
    return;
  }
  if (!IsWait(name) || !KeepRecord(wait_records)) return;
  std::lock_guard lock(event_mutex);
  Record("wait-after-original-sdk", name, &ctx);
}
}  // namespace gpu::host
