#!/usr/bin/env python3
"""Prove the one-request design against the app's real `bounded`, with SoC-like float means.

The plugin asks for twelve completed hours with one `recorder/statistics_during_period` request. Home Assistant
answers one key (the statistic id) whose list has up to thirteen hourly rows; the app's answer limit is 3.2 KB
and its `bounded` cuts dict keys to 32 bytes and lists from the end only when they do not fit. This check builds
that answer, runs it through Tessera's own `bounded`, and asserts the key was truncated while every row, including
the newest, survived. It also checks the manifest permission and that the code names one statistic id.

Run from the plugin folder:
    python3 tests/history_bounded_check.py
"""
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def tessera_app():
    """Tessera's screen_manager/app, from TESSERA_APP/TESSERA_TREE or the usual caches."""
    named = os.environ.get("TESSERA_APP")
    if named:
        return Path(named)
    tree = os.environ.get("TESSERA_TREE")
    if tree:
        return Path(tree) / "screen_manager" / "app"
    for base in (Path.home() / ".cache" / "tessera-plugins",):
        for candidate in base.rglob("tessera-main/screen_manager/app/plugins.py"):
            return candidate.parent
    raise SystemExit("TESSERA_APP or TESSERA_TREE must name a Tessera checkout (screen_manager/app)")


def main():
    app = tessera_app()
    sys.path.insert(0, str(app))
    import plugins  # the app's own bounded

    from datetime import datetime, timedelta, timezone

    now = datetime.now(timezone.utc).replace(minute=0, second=0, microsecond=0)
    start = now - timedelta(hours=12)
    statistic_id = "sensor.solarman_battery_state_of_charge_long_name"
    rows = []
    # HA's float means are long (47.833333333333336): the worst case for size.
    means = [47.833333333333336 + i * 3.1666666666666665 for i in range(12)]
    for i, mean in enumerate(means):
        begin = start + timedelta(hours=i)
        rows.append({"start": int(begin.timestamp() * 1000),
                     "end": int((begin + timedelta(hours=1)).timestamp() * 1000), "mean": mean})
    # A 13th row at the current hour is what a query that ends "now" can return; the code ignores it.
    rows.append({"start": int(now.timestamp() * 1000),
                 "end": int((now + timedelta(hours=1)).timestamp() * 1000), "mean": 99.99999999999999})
    raw = {statistic_id: rows}

    shaped = plugins.bounded(raw)
    assert isinstance(shaped, dict), shaped
    assert len(shaped) == 1, "bounded changed the one-statistic-id answer"
    key, got = next(iter(shaped.items()))
    assert len(rows) == 13 and len(got) == 13, f"bounded cut rows: {len(rows)} -> {len(got)}"
    assert len(key.encode()) == 32, f"bounded key is {len(key.encode())} bytes, expected 32"
    assert key != statistic_id, "the key was not truncated; this check proves nothing about key text"
    assert got[-1]["mean"] == 99.99999999999999, "the newest row was lost"
    assert [r["mean"] for r in got[:-1]] == means, "a row changed or was reordered"
    size = len(json.dumps(shaped, separators=(",", ":"), ensure_ascii=False).encode())
    assert size < 3200, f"the shaped answer is {size} bytes, over the app's 3.2 KB"
    # The shape our parser expects: one key, its value a list of objects with start/end/mean.
    assert all(isinstance(r.get("start"), int) and isinstance(r.get("end"), int) and
               isinstance(r.get("mean"), (int, float)) for r in got)

    manifest = (ROOT / "tessera-plugin.yaml").read_text()
    assert re.search(r"^\s*ha_commands:\s*\[\"recorder/statistics_during_period\"", manifest, re.M), \
        "the manifest does not name recorder/statistics_during_period"
    source = (ROOT / "components" / "power_origin" / "history_logic.h").read_text()
    assert 'doc["ask"] = "recorder/statistics_during_period";' in source
    assert 'data["statistic_ids"].to<JsonArray>().add(statistic_id);' in source
    assert "history/history_during_period" not in source, "the deprecated history command has another shape"

    if len(sys.argv) == 2:  # check_history.py hands this to the C++ parser
        Path(sys.argv[1]).write_text(json.dumps({"from": int(start.timestamp()), "result": shaped}))
    print(f"history_bounded_check: ok ({len(got)} rows, {size} bytes, key {len(key.encode())} bytes, "
          f"newest {got[-1]['mean']})")


if __name__ == "__main__":
    main()
