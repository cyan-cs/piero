"""Create new, disposable metadata-only folders. Never overwrite or delete data."""
from pathlib import Path
import sys

root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=False)
for i in range(30):
    folder = root / f"Folder_{i:02d}"
    folder.mkdir()
    for j in range(100):
        (folder / f"file_{j:03d}.txt").open("xb").close()
    (folder / "日本語_😀_e\u0301.txt").open("xb").close()
    (root / f"ordinary_{i:02d}.txt").open("xb").close()
large = root / "Large_20001"
large.mkdir()
for i in range(20001):
    (large / f"file_{i:05d}.txt").open("xb").close()
print(root.resolve())
