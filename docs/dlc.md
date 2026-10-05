# DLCs and Title Updates

[Documentation](README.md) · [Build guide](build-and-run.md)

The project implements installation and enumeration of additional content for Title ID **`555307E5`**, including STFS packages and already extracted folders. Content files must be supplied by the user.

## 1. Install through the launcher

1. Close the game and open `narutorise_launcher.exe`.
2. Open the **DLC** tab and select the folder containing your packages or extracted content.
3. Launch the game to complete installation.

The launcher recursively searches for packages with `CON `, `LIVE`, or `PIRS` signatures, even without a recognized extension. When packages are found, it copies them to `dlc/` next to `narutorise.exe`. The runtime installs them on the next launch and writes content metadata. If no STFS packages are found, it treats the selection as a folder of already extracted files.

The base game can run without DLC. For **character DLCs**, the distribution also needs `narutorise_ai2c2.dll`; copying content packages alone does not create this DLL.

## 2. Paths and manual installation

```text
<executable folder>/
├── narutorise.exe
├── narutorise_ai2c2.dll
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

### 3.1. Why are there two DLLs?

The base game uses the guest module `ai2c.dll`, recompiled as `narutorise_ai2c.dll`. Character content supplies another engine version, `AI2C@2.dll`, which must be recompiled separately.

### 3.2. Prepare the file

Extract `AI2C@2.dll` from your DLC package using an STFS-compatible tool. If you have already used a DLC-enabled build, it may also be present in the installed content folder in Documents.

Copy that file to **`game_root/ai2c2.dll`**. The local name without `@` produces a valid CMake target name; the guest name remains `ai2c@2.dll`.

### 3.3. Generate the recompiled module

The following block is already present in `app/narutorise_manifest.toml`:

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

Confirm that `app/out/build/win-amd64-release/narutorise_ai2c2.dll` exists. Packaging includes this DLL when available. It does not replace the DLC content files.

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

Search logs for `Installing DLC package`, `DLC auto-install`, `ContentManager lists`, and `DLC install failed`. If content appears installed but the game fails to load it, first check `narutorise_ai2c2.dll`, the SDK version, and the input file used for code generation.
