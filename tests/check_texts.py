#!/usr/bin/env python3
"""The screen's texts are printf formats: every language must hold the same keys with the same conversions."""
import json, pathlib, re
root = pathlib.Path(__file__).resolve().parents[1] / "translations"
spec = lambda t: re.findall(r"%[-0-9.]*[a-z%]", t)
en = json.loads((root / "en.json").read_text())["screen"]
for path in root.glob("*.json"):
    texts = json.loads(path.read_text())["screen"]
    assert texts.keys() == en.keys(), (path.name, texts.keys() ^ en.keys())
    for key, text in texts.items():
        assert spec(text) == spec(en[key]), (path.name, key)
print("check_texts: ok")
