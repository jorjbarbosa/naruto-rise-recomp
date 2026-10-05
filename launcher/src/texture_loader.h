#pragma once

#include <filesystem>
#include <d3d11.h>

struct LoadedTexture {
  ID3D11ShaderResourceView* srv = nullptr;
  int width = 0;
  int height = 0;

  void Release() {
    if (srv) {
      srv->Release();
      srv = nullptr;
    }
    width = 0;
    height = 0;
  }
};

LoadedTexture LoadTextureFromFile(ID3D11Device* device, const std::filesystem::path& path);
