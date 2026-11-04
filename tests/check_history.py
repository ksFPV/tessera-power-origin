#!/usr/bin/env python3
"""The history check: compile the host unit/async test and prove the bounded answer keeps 13 rows.

    python3 tests/check_history.py

Writes only to the scratch directory. It finds ArduinoJson in a local ESPHome/Tessera build or `ARDUINOJSON_DIR`.
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def arduinojson():
    named = os.environ.get("ARDUINOJSON_DIR")
    if named:
        return Path(named)
    bases = [Path.home()]
    tree = os.environ.get("TESSERA_TREE")
    if tree:
        bases.insert(0, Path(tree))
    for base in bases:
        if not base.is_dir():
            continue
        for found in base.rglob("ArduinoJson/src/ArduinoJson.h"):
            return found.parent
    raise SystemExit("ArduinoJson.h not found: set ARDUINOJSON_DIR to its src directory")


def main():
    include = arduinojson()
    with tempfile.TemporaryDirectory(prefix="power-origin-history-") as tmp:
        binary = Path(tmp) / "history_logic_test"
        print(f"compiling tests/history_logic_test.cpp (ArduinoJson {include}) ...")
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-pedantic", f"-I{include}",
                        str(ROOT / "tests" / "history_logic_test.cpp"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        shaped = Path(tmp) / "bounded.json"
        subprocess.run([sys.executable, str(ROOT / "tests" / "history_bounded_check.py"), str(shaped)], check=True)
        data = json.loads(shaped.read_text())
        result = Path(tmp) / "result.json"
        result.write_text(json.dumps(data["result"]))
        subprocess.run([str(binary), str(result), str(data["from"])], check=True)
    print("check_history: ok")


if __name__ == "__main__":
    main()
