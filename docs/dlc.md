# DLCs and Title Updates

[Documentation](README.md) · [Build guide](build-and-run.md)

The project implements installation and enumeration of additional content for Title ID **`555307E5`**, including STFS packages and already extracted folders. Content files must be supplied by the user.

## 1. Install through the launcher

1. Close the game and open `narutorise_launcher.exe`.
2. Open the **DLC** tab and select the folder containing your packages or extracted content.
3. Launch the game to complete installation.

The launcher recursively searches for packages with `CON `, `LIVE`, or `PIRS` signatures, even without a recognized extension. When packages are found, it copies them to `dlc/` next to `narutorise.exe`. The runtime installs them on the next launch and writes content metadata. If no STFS packages are found, it treats the selection as a folder of already extracted files.

The base game can run without DLC. Each **character DLC** requires its matching recompiled engine module; copying content packages alone does not create these DLLs.

| Character DLC | Guest engine | Recompiled module | XEX entry point |
| --- | --- | --- | --- |
| Shikamaru | `AI2C@1.dll` | `narutorise_ai2c1.dll` | `0x883A0CF8` |
| Jiraiya & Sarutobi | `AI2C@2.dll` | `narutorise_ai2c2.dll` | `0x883ADD48` |
| Choji & Temari | `AI2C@3.dll` | `narutorise_ai2c3.dll` | `0x883AEF88` |

Japanese Voices does not contain an engine DLL. When multiple character packs are installed, the game selects an engine revision; ship all three native modules to support every combination.

## 2. Paths and manual installation

```text
<executable folder>/
├── narutorise.exe
├── narutorise_ai2c1.dll
├── narutorise_ai2c2.dll
├── narutorise_ai2c3.dll
└── dlc/
    └── <user-supplied STFS packages>

<Documents>/narutorise/
└── 0000000000000000/
    └── 555307E5/
        ├── 00000002/
        │   └── <contents of each package>/
        └── Headers/
            └── 00000002/
                └── <.header metadata>
```

You can also place packages directly in `dlc/` and launch the game. `dlc_source_path` selects another source location; an empty string disables scanning that source, but does not remove installed content or disable its repair step.

Already installed packages are recognized by their content and corresponding header. The runtime also repairs content from earlier versions, long folder names, and missing headers. Check the log to confirm the result rather than treating the existence of a folder alone as proof of successful installation.

`tools/install-content.ps1` helps copy **already extracted files** and Title Updates; it does not extract STFS packages. The launcher + runtime workflow described above performs that extraction.

## 3. Build with the DLC module

### 3.1. Why are there separate engine DLLs?

The base game uses the guest module `ai2c.dll`, recompiled as `narutorise_ai2c.dll`. Character packs supply `AI2C@1.dll`, `AI2C@2.dll`, or `AI2C@3.dll`, each with a different code layout. They must be recompiled separately; renaming a native DLL from another revision is not a substitute.

### 3.2. Prepare the file

Extract the three engine DLLs from your DLC packages using an STFS-compatible tool. If you have already imported the DLCs, they may also be present in the installed content folder in Documents.

Copy `AI2C@1.dll` to **`game_root/ai2c1.dll`**, `AI2C@2.dll` to **`game_root/ai2c2.dll`**, and `AI2C@3.dll` to **`game_root/ai2c3.dll`**. Local names without `@` produce valid CMake target names; the guest names retain `@1`, `@2`, and `@3`.

### 3.3. Generate the recompiled module

All three mappings are already present in `app/narutorise_manifest.toml`, with a separate seed file for each revision. For example:

```toml
[[modules]]
guest_path = "ai2c@2.dll"
file_path = "../game_root/ai2c2.dll"
out_directory_path = "generated/ai2c2"
includes = ["ai2c2-seeds.toml"]
```

With the SDK installed and patches applied, run from the repository root:

```powershell
.\sdk\out\install\win-amd64\bin\rexglue.exe codegen .\app\narutorise_manifest.toml
$sdkPrefix = (Resolve-Path .\sdk\out\install\win-amd64).Path
cmake --preset win-amd64-release -S app -B app/out/build/win-amd64-release "-DCMAKE_PREFIX_PATH=$sdkPrefix"
cmake --build app/out/build/win-amd64-release
```

Confirm that `narutorise_ai2c1.dll`, `narutorise_ai2c2.dll`, and `narutorise_ai2c3.dll` exist in `app/out/build/win-amd64-release/`. Packaging builds and includes each generated revision and rejects missing module binaries. The DLLs do not replace DLC content files.

`patches/sdk/codegen-alt-version-dll-modules.patch` is part of this workflow. Avoid regenerating the project with `init --force`, which can remove the custom block. For a base-game-only build, see module selection in the [build guide](build-and-run.md).

## 4. Title Updates

The application configures an update folder mounted as `update:\`. It searches in this order:

1. A valid `update_data_root` in the TOML file next to the executable;
2. An explicit cvar/command-line path;
3. `title_update/` inside or alongside the game data folder;
4. `update/` in those same locations.

Example:

```toml
update_data_root = "D:/Games/Naruto/title_update"
```

Use **extracted** Title Update files. Mounting this folder does not mean executable updates are automatically applied to the already recompiled C++; compatibility depends on the game revision and modules used for the build. The repository does not currently provide a matrix of verified TUs and revisions.

## 5. Diagnostics

Search logs for `Installing DLC package`, `DLC auto-install`, `ContentManager lists`, and `DLC install failed`. Successful engine loading also logs `Registering module: ai2c@N.dll` and a function count. If content appears installed but the game fails to load it, check the matching native DLL, the SDK version, and the input file used for code generation.

Issue #7 was reproduced with Choji & Temari: an unsupported `AI2C@3.dll` caused `Execute(883AEF88): function not in function table`, followed by a fatal call to `0x88040000`. A build containing only the `@2` engine cannot run this pack. Setting `dlc_source_path = ""` stops new imports but does not disable installed DLCs; use an isolated user data directory when comparing character packs.

## 6. Startup regression test

With all four packs installed in a user-supplied content tree, run the opt-in
test from the repository root. Use a new output directory:

```powershell
python packaging/tests/test_dlc_runtime.py `
  --package app/out/build/win-amd64-release `
  --game "C:/path/to/extracted/game" `
  --content "$env:USERPROFILE/Documents/narutorise" `
  --output out/dlc-startup-test
```

The test copies binaries and marketplace content into isolated directories,
then starts the base game, each character engine separately, and all packs
together. Each case must register the expected engine and remain running for
30 seconds without a fatal function dispatch error. Logs and `results.json`
remain in the output directory. The test does not change installed DLCs,
settings, or saves; it checks startup rather than gameplay.
