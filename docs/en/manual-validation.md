[English](../en/manual-validation.md) | [日本語](../ja/manual-validation.md)

# Manual validation

Use an owned disposable Windows VM, isolated virtual test disks and a known
recoverable snapshot for filesystem, install/uninstall and interruption trials.
Do not attach host personal drives or shared user folders. Record the VM/disk
identities, snapshot, executable hashes and permitted test boundaries first.

Synthetic visual samples can be prepared without launching a terminal:

```powershell
cmake --build out/dev --target piero wic_proto
python tools/prepare_manual_validation.py --output out/manual-new --executable out/dev/piero.exe
```

Run the prepared launcher yourself in a real Windows Terminal, for example:

```powershell
.\out\manual-new\run-terminal.ps1 -Profile standard -Graphics auto -Language en
```

The launcher uses dedicated configuration and state directories. Its generated
results start as NOT RUN; fill them only after observing each case. File-operation
cases require the disposable VM described above. Check navigation,
prefetch, Japanese/English, IME/JIS, all keymaps, preview encodings, images/PDF,
Sixel/fallback, resize, dialog overlap, long idle, fast scrolling and terminal
restoration. A model or ConPTY result is not visual confirmation.

In the proven VM, compare originals, destinations, ADS, identities, backups,
temporaries and recovery state before and after operations. Check cancellation,
pause, denial, replacement races and damaged journals. Forbidden operations must
refuse; never enable them merely to test. Process crash, forced termination,
OS crash and VM power-off are different experiments. Stop on any mismatch and
preserve disks/logs rather than cleaning them automatically.

Install/uninstall tests must cover an absent, existing and externally modified
user PATH, duplicate/equivalent entries, registry string types, an occupied
install directory and retained personal configuration. Verify that portable and
installed executables report the same version. No real-machine installation or
network behavior is implied by the preparation scripts.
