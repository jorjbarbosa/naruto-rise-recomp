# Third-party components shipped in the release package. package-release.ps1
# copies each File into licenses\<Target> and lists it in licenses\README.txt.
# File is relative to the repository root; packaging\licenses holds verbatim
# copies of texts that only exist inside a source header or upstream repository.
# Condition selects a component only when that part is in the build:
#   FidelityFX     - amd_fidelityfx_dx12.dll is staged
#   NativeRenderer - NARUTORISE_NATIVE_RENDERER_BOOTSTRAP is ON
@{
    Components = @(
        @{ Name = 'nlohmann/json 3.11.3'; License = 'MIT'
           Url = 'https://github.com/nlohmann/json'; File = 'third_party/nlohmann_json/LICENSE.MIT'; Target = 'nlohmann-json.txt'
           Usage = 'narutorise.exe, narutorise_launcher.exe' }
        @{ Name = 'ReXGlue SDK (includes code derived from Xenia)'; License = 'BSD-3-Clause'
           Url = 'https://github.com/rexglue/rexglue-sdk'; File = 'sdk/LICENSE'; Target = 'rexglue-sdk.txt'
           Usage = 'narutorise.exe, rexruntime.dll, rexgpu-xenos.dll' }
        @{ Name = 'FFmpeg (libavcodec, libavutil)'; License = 'LGPL-2.1-or-later'
           Url = 'https://github.com/wmarti/FFmpeg'; File = 'sdk/thirdparty/FFmpeg/COPYING.LGPLv2.1'; Target = 'ffmpeg.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'libmspack (LZX decompressor)'; License = 'LGPL-2.1-only'
           Url = 'https://github.com/kyz/libmspack'; File = 'sdk/thirdparty/libmspack/libmspack/COPYING.LIB'; Target = 'libmspack.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'SDL3'; License = 'Zlib'
           Url = 'https://github.com/libsdl-org/SDL'; File = 'sdk/thirdparty/sdl3/LICENSE.txt'; Target = 'sdl3.txt'
           Usage = 'rexruntime.dll, narutorise_launcher.exe' }
        @{ Name = 'Dear ImGui'; License = 'MIT'
           Url = 'https://github.com/ocornut/imgui'; File = 'sdk/thirdparty/imgui/LICENSE.txt'; Target = 'imgui.txt'
           Usage = 'rexruntime.dll, narutorise_launcher.exe' }
        @{ Name = 'stb_image'; License = 'MIT OR Unlicense'
           Url = 'https://github.com/nothings/stb'; File = 'packaging/licenses/stb.txt'; Target = 'stb.txt'
           Usage = 'rexruntime.dll, narutorise_launcher.exe' }
        @{ Name = '{fmt}'; License = 'MIT'
           Url = 'https://github.com/fmtlib/fmt'; File = 'sdk/thirdparty/fmt/LICENSE'; Target = 'fmt.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'spdlog'; License = 'MIT'
           Url = 'https://github.com/gabime/spdlog'; File = 'sdk/thirdparty/spdlog/LICENSE'; Target = 'spdlog.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'SIMDe'; License = 'MIT'
           Url = 'https://github.com/simd-everywhere/simde'; File = 'sdk/thirdparty/simde/COPYING'; Target = 'simde.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'toml++'; License = 'MIT'
           Url = 'https://github.com/marzer/tomlplusplus'; File = 'sdk/thirdparty/tomlplusplus/LICENSE'; Target = 'tomlplusplus.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'CLI11'; License = 'BSD-3-Clause'
           Url = 'https://github.com/CLIUtils/CLI11'; File = 'sdk/thirdparty/cli11/LICENSE'; Target = 'cli11.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'xxHash'; License = 'BSD-2-Clause'
           Url = 'https://github.com/Cyan4973/xxHash'; File = 'sdk/thirdparty/xxHash/LICENSE'; Target = 'xxhash.txt'
           Usage = 'rexruntime.dll, rexgpu-xenos.dll' }
        @{ Name = 'o1heap'; License = 'MIT'
           Url = 'https://github.com/pavel-kirienko/o1heap'; File = 'sdk/thirdparty/o1heap/LICENSE'; Target = 'o1heap.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'disruptorplus'; License = 'MIT'
           Url = 'https://github.com/lewissbaker/disruptorplus'; File = 'packaging/licenses/disruptorplus.txt'; Target = 'disruptorplus.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'UTF8-CPP'; License = 'BSL-1.0'
           Url = 'https://github.com/nemtrif/utfcpp'; File = 'sdk/thirdparty/utfcpp/LICENSE'; Target = 'utfcpp.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'aes_128'; License = 'MIT'
           Url = 'https://github.com/openluopworld/aes_128'; File = 'sdk/thirdparty/aes_128/LICENSE'; Target = 'aes_128.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'DES (Faraz Fallahi)'; License = 'MIT'
           Url = 'https://github.com/fffaraz/cppDES'; File = 'sdk/thirdparty/crypto/des/LICENSE'; Target = 'des.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'TinySHA1'; License = 'ISC'
           Url = 'https://github.com/mohaps/TinySHA1'; File = 'packaging/licenses/tinysha1.txt'; Target = 'tinysha1.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'SHA-256 (Stephan Brumme)'; License = 'Zlib'
           Url = 'https://create.stephan-brumme.com/hash-library/'; File = 'packaging/licenses/sha256.txt'; Target = 'sha256.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'Rijndael reference implementation'; License = 'Public domain'
           Url = 'https://github.com/rexglue/rexglue-sdk/tree/main/thirdparty/crypto'; File = 'packaging/licenses/rijndael.txt'; Target = 'rijndael.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'NT image format header (pe_image.h)'; License = 'Notice'
           Url = 'https://github.com/rexglue/rexglue-sdk/tree/main/thirdparty/pe'; File = 'packaging/licenses/pe-image.txt'; Target = 'pe-image.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'RenderDoc in-application API header'; License = 'MIT'
           Url = 'https://github.com/baldurk/renderdoc'; File = 'packaging/licenses/renderdoc.txt'; Target = 'renderdoc.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'DirectX Shader Compiler API headers'; License = 'NCSA'
           Url = 'https://github.com/microsoft/DirectXShaderCompiler'; File = 'packaging/licenses/directx-shader-compiler.txt'; Target = 'directx-shader-compiler.txt'
           Usage = 'rexruntime.dll' }
        @{ Name = 'DXBC checksum (AMD; derived from the RSA Data Security, Inc. MD5 Message-Digest Algorithm)'; License = 'RSA-MD'
           Url = 'https://github.com/rexglue/rexglue-sdk/tree/main/thirdparty/dxbc'; File = 'packaging/licenses/dxbc-checksum.txt'; Target = 'dxbc-checksum.txt'
           Usage = 'rexgpu-xenos.dll' }
        @{ Name = 'extract-xiso'; License = 'BSD-4-Clause (modified)'
           Url = 'https://github.com/XboxDev/extract-xiso'; File = 'tools/extract-xiso/LICENSE.TXT'; Target = 'extract-xiso.txt'
           Usage = 'NarutoRiseInstaller.exe (ISO import)' }
        @{ Name = 'Lato fonts'; License = 'OFL-1.1'
           Url = 'https://www.latofonts.com'; File = 'launcher/assets/fonts/OFL.txt'; Target = 'lato.txt'
           Usage = 'assets\fonts' }
        @{ Name = 'AMD FidelityFX SDK'; License = 'MIT'; Condition = 'FidelityFX'
           Url = 'https://github.com/rexglue/FidelityFX-SDK'; File = 'packaging/licenses/fidelityfx.txt'; Target = 'fidelityfx.txt'
           Usage = 'amd_fidelityfx_dx12.dll' }
        @{ Name = 'plume'; License = 'MIT'; Condition = 'NativeRenderer'
           Url = 'https://github.com/renderbag/plume'; File = 'packaging/licenses/plume.txt'; Target = 'plume.txt'
           Usage = 'narutorise.exe' }
        @{ Name = 'D3D12 Memory Allocator'; License = 'MIT'; Condition = 'NativeRenderer'
           Url = 'https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator'; File = 'packaging/licenses/d3d12memoryallocator.txt'; Target = 'd3d12memoryallocator.txt'
           Usage = 'narutorise.exe' }
        @{ Name = 'XenosRecomp (shader translation, shader_common.h in the embedded shaders)'; License = 'MIT'; Condition = 'NativeRenderer'
           Url = 'https://github.com/hedge-dev/XenosRecomp'; File = 'tools/xenosrecomp/LICENSE.md'; Target = 'xenosrecomp.txt'
           Usage = 'narutorise.exe' }
        @{ Name = 'volk'; License = 'MIT'; Condition = 'NativeVulkan'
           Url = 'https://github.com/zeux/volk'; File = 'packaging/licenses/volk.txt'; Target = 'volk.txt'
           Usage = 'narutorise.exe (Vulkan loader)' }
        @{ Name = 'Vulkan Memory Allocator'; License = 'MIT'; Condition = 'NativeVulkan'
           Url = 'https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator'; File = 'packaging/licenses/vulkan-memory-allocator.txt'; Target = 'vulkan-memory-allocator.txt'
           Usage = 'narutorise.exe' }
        @{ Name = 'Vulkan Headers'; License = 'Apache-2.0'; Condition = 'NativeVulkan'
           Url = 'https://github.com/KhronosGroup/Vulkan-Headers'; File = 'packaging/licenses/vulkan-headers-apache.txt'; Target = 'vulkan-headers.txt'
           Usage = 'narutorise.exe and narutorise_launcher.exe' }
        @{ Name = 'SMOL-V'; License = 'MIT and Apache-2.0'; Condition = 'NativeVulkan'
           Url = 'https://github.com/aras-p/smol-v'; File = 'tools/xenosrecomp/thirdparty/smol-v/license.txt'; Target = 'smol-v.txt'
           Usage = 'narutorise.exe (embedded SPIR-V decoder)' }
    )
}
