[English](../en/packaging.md) | [日本語](../ja/packaging.md)

# Windows packages

Builds and packages produce piero.exe; packages also contain the per-user PATH
helper, `languages/`, MIT LICENSE and the unchanged third-party LICENSES folder. The package
builder requires an unsanitized Release build with a static CRT. It inventories
normal and delayed DLL imports and refuses unreviewed non-system dependencies.
Do not copy arbitrary DLLs to make a package start.

From x64 Developer PowerShell, choose new directories under out:

```powershell
powershell -NoProfile -File tools/package_windows.ps1 -Version 0.0.3-dev -BuildDirectory out/package-build-new -OutputDirectory out/package-new
python tools/check_package.py out/package-new/payload --executable piero.exe --installer-helper
```

This creates `piero-windows-x64.zip` and a hash/version manifest with the exact
installer payload. The version shown is a development candidate, not a released
stable version. The owner chooses the final version when qualification is complete.
Build versions use SemVer with at most 63 ASCII characters and 32-bit core numbers,
matching the update checker's supported range.
The ZIP can be unpacked into a new directory and launched without installation.

Inno Setup with x64compatible support must be on PATH to also compile setup:

```powershell
powershell -NoProfile -File tools/package_windows.ps1 -Version 0.0.3-dev -BuildDirectory out/installer-build-new -OutputDirectory out/installer-new -Installer
```

The output is `piero-Setup-x64.exe`. It requires Windows 11 (build 22000 or later). It installs in the user's Programs directory,
requires no elevation, registers uninstall and adds a Start Menu shortcut. Optional
user-PATH registration is unchecked by default. Restart the terminal after adding it.
The helper preserves the existing registry string type, avoids equivalent entries,
and records the exact append it owns. Uninstall restores only a matching append
snapshot. If another program changed PATH, it leaves PATH untouched and asks for
manual review. It never edits machine PATH or recursively removes personal state.
Setup refuses an occupied destination or an existing registered installation;
uninstall the previous application first. It does not close running applications.
Registry comparison is not an atomic lock against unrelated PATH writers; qualify
installation/concurrent changes in a disposable VM before distributing setup.

The builder verifies ZIP bytes against the installer payload and version. Installer
compilation and actual install/uninstall are separate checks. No code-signing key
is configured: local outputs are unsigned unless independently signed and verified.
SmartScreen may warn; do not disable Windows protection. Qualify clean-machine
startup, PATH changes, upgrades and uninstall in a disposable snapshot-backed VM.
Check the official Releases page for availability; local package creation does not publish a release.

Syntax colouring is optional. The current ZIP builder and Inno Setup file do
not include the build's `syntax` directory. To distribute grammars, prepare that
directory separately from the same build as `piero.exe`, retain its notices and
verify its files. Config and Extended pack groupings describe installation
layouts, not additional archives produced by `package_windows.ps1`.
See [syntax colouring](syntax.md) for the layout and hash checks.
