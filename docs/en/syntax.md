[English](../en/syntax.md) | [日本語](../ja/syntax.md)

# Syntax colouring

piero colours text in the preview pane and in the built-in editor with Tree-sitter grammars. The grammars are not part of `piero.exe`. Each one is a separate DLL that you copy next to the program, and a file is coloured only when the grammar for it is installed. Without any grammar nothing changes: text is shown plain, and the editor keeps the simple colouring it always had.

## What is coloured

| Language | Files |
| --- | --- |
| JSON | `.json`, `.geojson`, `.webmanifest` |
| JSONC | `.jsonc`, `.code-workspace`, `tsconfig.json`, `jsconfig.json`, `settings.json`, `launch.json`, `tasks.json`, `extensions.json` |
| YAML | `.yaml`, `.yml` |
| TOML | `.toml`, `Cargo.lock`, `Pipfile` |
| INI and CFG | `.ini`, `.cfg`, `.inf`, `.desktop`, `.url`, `.editorconfig`, `.gitconfig`, `.gitmodules` |
| XML | `.xml`, `.xsd`, `.xsl`, `.xslt`, `.svg`, `.xaml`, `.csproj`, `.vcxproj`, `.props`, `.targets`, `.plist`, `.config`, `.manifest`, `.resx`, `.nuspec`, `.rss`, `.atom`, `.wsdl` |
| Markdown | `.md`, `.markdown` (the preview pane draws Markdown rendered, so only the editor colours its source) |
| PowerShell | `.ps1`, `.psm1`, `.psd1` |
| Shell | `.sh`, `.bash`, `.bashrc`, `.bash_profile`, `.bash_aliases`, `.profile`, `PKGBUILD` |
| dotenv | `.env`, `.env.local`, `.env.example`, `.env.development`, `.env.production`, `.env.test` |

A whole file name wins over the extension, so `tsconfig.json` is JSONC and other `.json` files are JSON. INI is the dialect the grammar reads: `[sections]`, `name = value` and comments, with the values left plain. dotenv has no grammar DLL: a lexer inside `piero.exe` colours it, because one shape of line does not need a parser.

The colour of each kind of text (keys, strings, numbers, booleans, null, comments, headings, links and so on) comes from the theme, for every preset and the custom theme. Changing the theme repaints the colours and parses nothing again.

## Installing

Copy the optional files from the same build next to `piero.exe`. The current
package scripts do not ship grammar packs; see [packaging](packaging.md).
Use a folder writable by your account.

```text
piero.exe
syntax\piero-syntax.dll
syntax\grammars\json\parser.dll
syntax\grammars\json\highlights.scm
syntax\grammars\json\manifest.toml
syntax\grammars\toml\...
```

| Package | What is in it |
| --- | --- |
| Core | Base package without the optional `syntax` folder; no grammar colouring. |
| Config pack | `syntax\piero-syntax.dll` and the grammars `json`, `jsonc`, `yaml`, `toml`, `ini`, `xml`. |
| Extended pack | `syntax\piero-syntax.dll` and the grammars `markdown`, `markdown_inline`, `powershell`, `bash`. |
| One grammar | `syntax\piero-syntax.dll` and one folder of `syntax\grammars\`. |

`piero-syntax.dll` is in every pack, so a single pack is complete. Installing a second pack replaces it with the same file. `markdown_inline` is the second grammar of Markdown (emphasis, code spans and links); without it Markdown is coloured by block (headings, fences, lists) and the inline markup stays plain. To remove a grammar, delete its folder. A grammar that is loaded is not touched by piero, and a deleted one is simply not found at the next start.

The executable, syntax DLL and grammar files must come from the same build.
Even the same version rebuilt with another compiler can have different hashes.
Replace the whole `syntax` folder together with the matching executable when updating.

## How it behaves

- Nothing is loaded at start. Listing, searching and fuzzy find never load a grammar. The first time a file is shown whose grammar is installed, a worker thread loads `piero-syntax.dll` and then only that grammar. The grammars of languages you do not look at are never loaded, whatever is installed.
- The text is shown at once, plain, and the colours arrive when the worker answers. Moving to another file drops the colours of the old one.
- In the editor the colours follow what you type: the text is handed to the worker after a change (never for a cursor move), the old colours stay in place, shifted over the edit, until the new ones arrive, and the parser reuses its old tree. Undo and Redo are changes of the text like any other. Colours never change the text, its encoding, its line ends or what is saved.
- Loaded grammars are kept until piero exits and are then released together; there is no unloading while a worker could still be using one.
- If a grammar cannot be used (missing, refused, broken, too slow) the text is shown plain and editing is not affected.

## Limits

Text over 2 MiB, a line over 64 KiB, a parse that takes more than 400 ms, a query that takes more than 200 ms, or more than 1,000,000 coloured runs: that text is not coloured, and it is shown and edited as before. The preview pane colours the text it reads: 256 KiB by default, configurable up to 1024 KiB through `[preview] text_max_kib`.

Filename detection currently accepts only ASCII basenames shorter than 64 characters.
Other names, including Japanese filenames, use plain preview text or the editor's
built-in colouring rather than the optional grammar.

## Safety

A grammar DLL is native code that runs with the rights of piero. These steps make a wrong or partly replaced installation fail; they do not make a malicious DLL safe.

- Only the grammars pinned by this build can be loaded. `piero-syntax.dll` carries the SHA-256 of each official `parser.dll`, `highlights.scm` and `manifest.toml`, and `piero.exe` carries the SHA-256 of `piero-syntax.dll`. A file that does not match is never loaded and the refusal is kept until the next start.
- `manifest.toml` describes a grammar and adds nothing: it must agree with the table inside the DLL, and it is read strictly (size, length, duplicate and unknown keys, file names).
- The folder is the folder of the running `piero.exe`; the current directory and PATH are never searched. A path with a symbolic link, junction or mount point below it, a network path, or a drive that is not local is refused. While a file is checked and loaded it is held open so that nobody can change it in between.
- The DLL search is limited to System32 and the folder of the DLL; the grammars import only `kernel32`.
- Grammar and DLL ABI versions are checked after loading. That is a consistency check of a file piero already decided to run, not a protection.
- With a portable install in a folder you can write to, anyone who can write there can also replace `piero.exe`. The checks then protect only against a partial change (a dropped grammar, a pack of another version). There is no code signing yet.
- Only grammars pinned by this build are accepted; custom grammars are not supported.

## Not done

Grammars for C/C++, Python and other languages outside the table above, loading a grammar that a manifest names, other languages inside Markdown code fences, colouring of the other previews (CSV, pictures), and unloading a grammar while piero runs.
