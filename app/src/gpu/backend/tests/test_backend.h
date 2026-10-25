// The backend an engine host test runs on: REX_NATIVE_TEST_API=d3d12 (the
// default) or vulkan. The tests check the common contract of the engine; an
// unknown value leaves the API unset, so Initialize refuses it.
#pragma once

#include <cstdlib>
#include <string_view>
#include "gpu/backend/contracts.h"

namespace gpu {

// Sets a variable of the process environment for the engine under test.
inline void SetTestEnvironment(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}
inline RenderMode TestRenderMode() {
  RenderMode mode;
  const char* api = std::getenv("REX_NATIVE_TEST_API");
  if (api && std::string_view(api) == "vulkan") mode.api = GraphicsApi::kVulkan;
  else if (api && *api && std::string_view(api) != "d3d12") mode.api = GraphicsApi(0xFF);
  return mode;
}

}  // namespace gpu
