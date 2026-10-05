#include "texture_loader.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

LoadedTexture LoadTextureFromFile(ID3D11Device* device, const std::filesystem::path& path) {
  LoadedTexture tex;
  if (!device || !std::filesystem::exists(path)) {
    return tex;
  }

  int channels = 0;
  unsigned char* data = stbi_load(path.string().c_str(), &tex.width, &tex.height, &channels, 4);
  if (!data) {
    return tex;
  }

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = static_cast<UINT>(tex.width);
  desc.Height = static_cast<UINT>(tex.height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  D3D11_SUBRESOURCE_DATA init_data = {};
  init_data.pSysMem = data;
  init_data.SysMemPitch = static_cast<UINT>(tex.width * 4);

  ID3D11Texture2D* pTexture = nullptr;
  HRESULT hr = device->CreateTexture2D(&desc, &init_data, &pTexture);
  if (SUCCEEDED(hr) && pTexture) {
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(pTexture, &srv_desc, &tex.srv);
    pTexture->Release();
  }

  stbi_image_free(data);
  return tex;
}
