"""Structural inventory, not an AST analyzer or a proof of code quality."""
from pathlib import Path
import json
import re
import sys

root = Path(__file__).resolve().parent.parent
output = Path(sys.argv[1])
files = [p for area in ("src", "include", "tests", "tools", "docs", "cmake")
         if (root / area).exists() for p in (root / area).rglob("*")
         if p.is_file() and "third_party" not in p.parts and "__pycache__" not in p.parts]
files += [root / "CMakeLists.txt"]
inventory, functions, dependencies = [], [], []
strip = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
definition = re.compile(r"^([\w *]+?)\s+(\w+)\s*\([^;{}]*\)\s*\{", re.M)
for path in sorted(files):
    if path.suffix.lower() not in (".c", ".h", ".py", ".ps1", ".md", ".cmake", ".txt"):
        continue
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    name = path.relative_to(root).as_posix()
    inventory.append({"file": name, "lines": len(text.splitlines()), "bytes": path.stat().st_size})
    if path.suffix not in (".c", ".h"):
        continue
    code = strip.sub(lambda m: "\n" * m.group().count("\n"), text)
    for match in definition.finditer(code):
        if match[2] in ("if", "for", "while", "switch"):
            continue
        start, depth, end = match.end() - 1, 1, match.end()
        while depth and end < len(code):
            depth += (code[end] == "{") - (code[end] == "}")
            end += 1
        body = code[start:end]
        functions.append({"file": name, "function": match[2],
                          "line": code[:start].count("\n") + 1,
                          "lines": body.count("\n") + 1,
                          "decisions": len(re.findall(r"\b(?:if|for|while|case)\b|&&|\|\||\?", body))})
    for include in re.findall(r'^\s*#\s*include "([^"]+)"', text, re.M):
        candidates = (path.parent / include, root / "src" / include)
        target = next((p.resolve() for p in candidates if p.is_file()), None)
        if target is not None and target.is_relative_to(root):
            dependencies.append([name, target.relative_to(root).as_posix()])
data = {"method": "lexical approximation; comments/strings stripped; vendor sources excluded",
        "files": inventory, "functions": sorted(functions, key=lambda x: x["decisions"], reverse=True),
        "local_includes": dependencies}
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
print(f"{len(inventory)} files, {len(functions)} recognized function bodies, {len(dependencies)} include edges")
for item in data["functions"][:15]:
    print(f"{item['file']}:{item['line']} {item['function']}: {item['lines']} lines, {item['decisions']} decision tokens")
