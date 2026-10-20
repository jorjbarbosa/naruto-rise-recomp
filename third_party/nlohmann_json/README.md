# nlohmann/json 3.11.3

The application vendors the single-header JSON parser and its MIT license here.
The game, launcher and package validator use the `narutorise::json` CMake target.
Building these targets does not depend on the SDK's internal inja directory or
download this dependency.

The initial header and license came from the SDK's existing
`thirdparty/inja/third_party/include/nlohmann` distribution. The header identifies
version 3.11.3. SHA-256 of `include/nlohmann/json.hpp` with LF line endings:

```text
9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
```

Update this version record and the license together when replacing the header.
