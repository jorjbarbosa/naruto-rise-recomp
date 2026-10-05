# extract-xiso (bundled binary)

Prebuilt `extract-xiso.exe` from the official [XboxDev/extract-xiso](https://github.com/XboxDev/extract-xiso)
releases, used only by the installer wizard
to extract original Xbox 360 XISO images.

- Binary: `extract-xiso-Win64_Release.zip` from release tag
  `build-202609111233` (v2.7.1, win64)
- License: see [LICENSE.TXT](LICENSE.TXT) (BSD-style, by in <in@fishtank.com>,
  maintained by the XboxDev organization). Redistribution with the notice is
  explicitly permitted.
- Upstream source: https://github.com/XboxDev/extract-xiso

The CMake build copies this binary into the development build for helper tests.
Inno Setup embeds it and runs it only in its private temporary directory. It is
not installed or included in the portable ZIP. The installer invokes a headless C++ helper for title
validation, progress and cancellation; the original game ISO is never bundled.
