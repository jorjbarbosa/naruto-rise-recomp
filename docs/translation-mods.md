# Translation mods

[Documentation](README.md) · [Configuration](configuration.md)

Translation mods replace individual text IDs when the game loads its English text tables. The loader builds a temporary table in guest memory; the game copies it into its own text pool. `naruto.bf` stays unchanged, and IDs missing from the mod keep their original text.

## Bundled Brazilian Portuguese translation

Releases include the complete 6,453-ID package from [`mods/pt-br`](../mods/pt-br).
ZIP and AppImage place the immutable seed in `defaults/mods/pt-br`.
Preparation copies it to `<active data folder>/mods/pt-br` when that package is
absent, after importing eligible old mods. Find the active folder in
**Settings → Advanced**. The menu and runtime load only this active `mods/` path.
Choose **Português (Brasil)** in **Settings → Game → Game language** to activate it.
Installing or updating does not change the selected language.

Initialization, rebuilds, ZIP/AppImage updates and the compatibility bridge
preserve active packages in full, including customized or partial PT-BR packages.
Updates refresh only the immutable seed. To update an active translation, close
the game, back it up and merge or replace it with the new seed deliberately.
You can also customize a separate package and select it in the ingame menu.
See [data migration and recovery](data-folder.md) for legacy imports, conflicts,
portable mode and isolated session overrides.

## Try the Portuguese demo

Copy [examples/mods/pt-br-demo](../examples/mods/pt-br-demo) into `mods/pt-br-demo`
inside the active data folder:

```text
<active data folder>/
narutorise.toml
mods/
  pt-br-demo/
    mod.toml
    textos.json
```

Open **Settings → Game → Game language** and choose the demo. Valid packages in
the active `mods/` appear by their manifest name. Copying a package makes it
available; selecting it activates it after restart. Reopen the menu to refresh
the list after installing a package.

Selecting a translation automatically uses English as its base. Selecting an
official language clears the translation selection. The active TOML preserves
the selected package across restarts and data-folder changes. A missing or
invalid package is omitted from the list; the runtime reports it and retains
original English text. Its saved selection remains until you choose another
language. Package structure and JSON are validated before loading; engine tags
and arguments are checked when the game loads each table.

For manual configuration, close the game and set these keys in the active data folder’s `narutorise.toml` (Settings → Advanced):

```toml
user_language = 1
translation_mod = "pt-br-demo"
```

Restart the game after editing either the configuration or the mod. Set `translation_mod = ""` to disable it. The demo translates five IDs covering three labels: **OPTIONS → OPÇÕES**, **Subtitles → Legendas**, and **GAME PAUSED → JOGO PAUSADO**. It is a small test package, not a complete Portuguese translation.

During development, CMake copies the factory template to `defaults/narutorise.toml`
and the two curated PT-BR files to `defaults/mods/pt-br`. It preserves active
preferences and mods. Personal settings and translations belong in the active
data folder. See [Data folder and recovery](data-folder.md).

## Create a package

Use a folder name containing only letters, digits, hyphens or underscores, with at most 80 characters. Only one translation package can be selected at a time. Its `mod.toml` declares the English base and the JSON filename:

```toml
[mod]
type = "translation"
name = "My translation"
version = "0.1.0"
base_language = "en"
texts = "textos.json"
```

Save `textos.json` as UTF-8, with or without a BOM. Keys are decimal text IDs, and values are translated strings:

```json
{
  "15132": "OPÇÕES",
  "19369": "Legendas",
  "21102": "JOGO PAUSADO"
}
```

Preserve engine commands such as `<br>`, `<orange>`, `<x>` and their closing tags. Preserve the numbered arguments `%1`, `%2`, etc., including repeated occurrences; their order may change to suit the translation. For example, `Unlock %1 in exchange for %2 Gold Coins?` can become `Por %2 moedas de ouro, desbloquear %1?`. A percentage such as `100%` remains ordinary text.

Strings may grow: the loader recalculates lengths and offsets. ASCII and printable Latin-1, including Portuguese accents, use one byte where possible. Typographic quotes, dashes, ellipses and other characters outside that range use UTF-16; existing wide entries remain wide. Unchanged original text retains its exact bytes, including legacy `0x85` ellipses. Glyph availability and text fitting still depend on the game's fonts and layout. A JSON string cannot contain a NUL character or exceed 65,535 UTF-16 units. Files and rebuilt tables are limited to 16 MiB.

The loader preserves text IDs, auxiliary fields, table order, and the encoded bytes of untranslated entries. Invalid JSON or a missing package leaves the original texts active. An entry with changed engine tags or numbered arguments keeps only that ID's original text; other valid translations in the table are applied. Each rejected ID and its reason are logged. Structural TXU errors or a rebuilt table exceeding the byte limit still retain the entire original table.

UTF-16 does not add missing glyphs to the game's fonts. In the Options smoke test
on 2026-10-20, typographic quotes and `œ` appeared, but en/em dashes and an ellipsis
did not appear in that label font, on either renderer. The encoder's exact
UTF-16LE units are covered by the host tests; font coverage remains a separate
limit.

## Russian terminology and subtitle references

The initial Russian project glossary is in [`localization/ru`](../localization/ru).
It uses the user-provided **Alex & Julia** Russian subtitles for Naruto episodes
**1–81**. Each supported entry includes selected excerpts with episode, ASS line
number, timestamp and source-file hash. `subtitle-sources.json` identifies all 81
input files; the full subtitle corpus remains local.

Online research for the seven initially unresolved entries is recorded separately
in `web-research.json`, including source URLs, access method, corroborated names
and editorial decisions. Official game/translator sources are distinguished from
community references for Russian spelling. Palm Healing remains unresolved as a
specific named-technique equivalence; the literal game label itself is verified.

The glossary distinguishes preferred wording, contextual equivalents and unresolved
candidates. Subtitle wording does not by itself establish correspondence with an
English game technique. Notes record composed game labels, source variants and
conflicts, including the Lotus names. Interface terms are explicitly marked as
project conventions. These choices do not claim an official Russian localization.

Rebuild the local subtitle index and the spreadsheet/AI exports with:

```powershell
python -I tools/localization/extract-subtitle-corpus.py "path/to/Russian ASS subtitles" out/ru-subtitles --first 1 --last 81
python -I tools/localization/export-glossary.py --locale ru
```

The subtitle index supports terminology review; it does not automatically translate
or align English and Russian dialogue. Translate from the English game export and
preserve engine commands and arguments as described above. Cyrillic encoding is
supported by the TXU codec, but the 2026-10-20 Xenos menu probe displayed only the
Latin prefix of the supplied Russian label. Cyrillic font coverage and text fit
must be addressed before claiming a playable Russian package.

## Validate a complete translation

Compare the finished dictionary with the original ID/text export before packaging:

```powershell
python -I tools/localization/validate-translation.py path/to/english-source-order.json path/to/mod/texts.json --report out/translation-validation.json
```

The validator rejects missing or unexpected IDs, duplicate JSON keys, empty translations, invalid Unicode, oversized strings, and changed engine tokens or numbered arguments. It also reports unchanged strings and translations that differ for identical original text. Those candidates require contextual judgment; names, service brands and vocalizations can legitimately remain unchanged. A passing structural check does not establish translation quality or text fit in the game.

To validate a complete package with the same TXU parser and encoder as the game,
build and run the CMake validator:

```powershell
cmake --build app/out/build/win-amd64-release --target narutorise_validate_translation_package
app/out/build/win-amd64-release/narutorise_validate_translation_package.exe mods/pt-br path/to/fixtures out/package-validation
```

The fixture directory must contain the extracted English tables `_fd141210.txu`
and `_fd190232.txu`. The validator checks every ID and the rebuilt bytes, writes
the rebuilt tables and `txu-validation.json`, and fails if any entry is rejected.
Unlike the tolerant runtime loader, this is a strict check before distribution.
Game fixtures are not shipped in the repository.

The JSON parser is pinned in `third_party/nlohmann_json` and shared through the
`narutorise::json` CMake target, independently of the SDK's private inja headers.

## Scope and diagnostics

The current loader targets the two English TXU resources found in the base archive: `AB00018A` (6,319 texts) and `5400047A` (134 texts). Set the game language to English (`user_language = 1`); another language leaves the mod inactive. DLC-specific text tables, text baked into textures, voices and fonts need separate support.

Logs prefixed with `[translation]` report package loading, each rejected ID, table replacement counts, and release of the temporary input. The copy into the game's own pool is established by the callback contract documented in `app/src/mods/translation_mod.cpp`; it is not rechecked by a runtime `VerifyCopy` pass. Loading a package alone does not prove that every supplied ID was found or that every translated screen has been visited.

The demo was exercised with both renderers: **OPÇÕES** appears in the native renderer and Xenos, and Xenos also shows **LEGENDAS** and **JOGO PAUSADO**. The native renderer currently omits the subtitles and vibration labels even with the mod disabled; that existing rendering issue also affects this demo.

The parser can be checked without launching the game:

```powershell
cmake --build app/out/build/win-amd64-release --target narutorise_translation_test
app/out/build/win-amd64-release/narutorise_translation_test.exe out/translation-tests
```

This runs synthetic tests for table rebuilding, encoding, invalid input, package paths and engine arguments. An optional second argument supplies a directory containing the fourteen extracted TXU fixtures for byte-preservation checks. Game assets and these fixtures are not distributed with the mod.
