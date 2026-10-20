# Translation mods

[Documentation](README.md) · [Configuration](configuration.md)

Translation mods replace individual text IDs when the game loads its English text tables. The loader builds a temporary table in guest memory; the game copies it into its own text pool. `naruto.bf` stays unchanged, and IDs missing from the mod keep their original text.

## Bundled Brazilian Portuguese translation

Releases include the complete 6,453-ID package from [`mods/pt-br`](../mods/pt-br).
Both the portable ZIP and Setup place it in `mods/pt-br` next to the executable;
Setup also adds or refreshes it when updating an existing installation.
Choose **Português (Brasil)** in **Settings → Game → Game language** to activate it.
Installing or updating does not change the selected language.

Updates replace the bundled `mod.toml` and `texts.json`, leaving other mod folders
and extra files intact. To customize this translation, copy it to another mod
folder and select that copy in the launcher.

## Try the Portuguese demo

Copy [examples/mods/pt-br-demo](../examples/mods/pt-br-demo) into `mods/pt-br-demo` next to `narutorise.exe`:

```text
narutorise.exe
narutorise.toml
mods/
  pt-br-demo/
    mod.toml
    textos.json
```

Open the launcher and choose the demo in **Settings → Game → Game language**. Valid packages in `mods/` appear by their manifest name, without added labels. Copying a package makes it available; only selecting it activates it. The launcher refreshes the list when opened, when its window regains focus, and before launching the game.

Selecting a translation automatically uses English as its base. Selecting any official language disables the translation. The launcher remembers the package selection on the next opening. If the selected package is missing or invalid, it disappears from the list and the launcher automatically selects English and clears the mod selection, without a warning. Restoring the package makes it available again but does not activate it. The launcher checks package structure and JSON, while engine tags and arguments are still checked by the game when it loads each table.

For manual configuration, close the game and launcher and set these keys in that executable's `narutorise.toml`:

```toml
user_language = 1
translation_mod = "pt-br-demo"
```

Restart the game after editing either the configuration or the mod. Set `translation_mod = ""` to disable it. The demo translates five IDs covering three labels: **OPTIONS → OPÇÕES**, **Subtitles → Legendas**, and **GAME PAUSED → JOGO PAUSADO**. It is a small test package, not a complete Portuguese translation.

During development, CMake copies `app/narutorise.toml` over the configuration in the build output. Keep your preferred development setting in the template, or test in a separate executable folder.

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
limit. Local captures: [Xenos](../out/translation-fixes-20261020-082055/mixed-xenos/options.png)
and [native](../out/translation-fixes-20261020-082055/mixed-native/options.png).

## Shared terminology for translation

The researched Brazilian Portuguese glossary lives in [localization/pt-BR/glossary.json](../localization/pt-BR/glossary.json). Each entry records its sources, aliases, decision basis and usage notes. Licensed anime, publisher and game references take priority; community references are identified explicitly. The project preferences are not a claim that every Brazilian release uses the same wording.

The user's selected terms are **Aldeia da Folha**, **Possessão das Sombras**, and the extended **Jutsu de Possessão das Sombras**. These decisions take precedence over other recorded variants. The English source typo `Shadow Posession Jutsu` is included as an alias.

Use [glossary.csv](../localization/pt-BR/glossary.csv) in a spreadsheet or map its `en` and `pt-BR` columns when importing into a translation platform. It uses UTF-8 with BOM and a semicolon separator. The status column distinguishes current preferences, context-dependent terms and unresolved candidates. Filter the status before importing a fixed terminology list.

Supply [ai-context.json](../localization/pt-BR/ai-context.json) as shared context when translating batches. It separates preferred terms from conditional adaptations and unresolved candidates. For example, `Guy` names a character only in the appropriate sense, and `Believe it` becomes the catchphrase only when the line supports that reading. Full technique names take precedence over generic terms; a glossary entry is not a blind substring replacement.

After editing the main JSON, regenerate the CSV and AI context:

```powershell
python -I tools/localization/export-glossary.py
```

The optional `--catalog path/to/english-texts.json` argument produces a local `glossary-coverage.json` beside an extracted catalog, containing matching text IDs. These lexical matches identify review candidates; they do not establish speaker identity or scene order. The mod's runtime texts file remains a decimal ID to translated string mapping.

## Validate a complete translation

Compare the finished dictionary with the original ID/text export before packaging:

```powershell
python -I tools/localization/validate-translation.py path/to/english-source-order.json path/to/mod/texts.json --glossary localization/pt-BR/glossary.json --report out/translation-validation.json
```

The validator rejects missing or unexpected IDs, duplicate JSON keys, empty translations, invalid Unicode, oversized strings, and changed engine tokens or numbered arguments. It also reports unchanged strings and translations that differ for identical original text, plus candidates that need checking against the user's terminology choices. Those candidates require contextual judgment; names, service brands and vocalizations can legitimately remain unchanged. A passing structural check does not establish translation quality or text fit in the game.

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
