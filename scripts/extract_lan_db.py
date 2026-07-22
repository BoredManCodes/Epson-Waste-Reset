#!/usr/bin/env python3
"""
Extract the resettable-model database from the Ircama epson_print_conf.py
PRINTER_CONFIG dict into a flat lan_database.json for the native C++ EWR GUI.

Only the fields needed to replay a waste-ink reset over SNMP are kept:
  read_key, write_key, and the ordered (address, value) reset sequence.

Uses the ast module so it needs none of epson_print_conf's runtime deps
(pysnmp, pyprintlpr, epson_escp2). Non-literal fields (range(), itertools,
stats, serial_number, ...) are ignored; the reset path never uses them.
"""
import ast
import json
import sys

SRC = sys.argv[1]
OUT = sys.argv[2]

with open(SRC, "r", encoding="utf-8") as f:
    tree = ast.parse(f.read())

# Find the PRINTER_CONFIG = {...} assignment inside class EpsonPrinter.
config_node = None
for node in ast.walk(tree):
    if isinstance(node, ast.Assign):
        for t in node.targets:
            if isinstance(t, ast.Name) and t.id == "PRINTER_CONFIG":
                config_node = node.value
if config_node is None or not isinstance(config_node, ast.Dict):
    sys.exit("PRINTER_CONFIG dict not found")


def lit(node):
    """literal-eval a node, or None if it isn't a pure literal (range/itertools)."""
    try:
        return ast.literal_eval(node)
    except Exception:
        return None


# Raw parse: model name -> {field: value} for the fields we understand.
raw = {}
for k_node, v_node in zip(config_node.keys, config_node.values):
    name = ast.literal_eval(k_node)
    if not isinstance(v_node, ast.Dict):
        continue
    entry = {}
    for fk, fv in zip(v_node.keys, v_node.values):
        field = ast.literal_eval(fk)
        if field in ("read_key", "write_key", "raw_waste_reset",
                     "main_waste", "borderless_waste", "alias", "same-as"):
            entry[field] = lit(fv)
    raw[name] = entry

# Resolve "alias": duplicate the entry under each alias name.
for name in list(raw.keys()):
    aliases = raw[name].pop("alias", None)
    if aliases:
        for a in aliases:
            if a not in raw:
                raw[a] = dict(raw[name])

# Resolve "same-as": merge referenced entry underneath (self wins).
for name in list(raw.keys()):
    sameas = raw[name].get("same-as")
    if sameas and sameas in raw:
        merged = dict(raw[sameas])
        merged.update(raw[name])
        raw[name] = merged


def reset_sequence(entry):
    """Replicate reset_waste_ink_levels(): raw path, else main/borderless path."""
    rwr = entry.get("raw_waste_reset")
    if isinstance(rwr, dict) and rwr:
        return [[int(a), int(v)] for a, v in rwr.items()]
    seq = []
    mw = entry.get("main_waste")
    if isinstance(mw, dict) and mw.get("oids"):
        seq += [[int(o), 0] for o in mw["oids"]]
        bw = entry.get("borderless_waste")
        if isinstance(bw, dict) and bw.get("oids"):
            seq += [[int(o), 0] for o in bw["oids"]]
    return seq


models = []
seen = set()
for name, entry in sorted(raw.items()):
    rk = entry.get("read_key")
    wk = entry.get("write_key")
    seq = reset_sequence(entry)
    if not (isinstance(rk, (list, tuple)) and len(rk) == 2):
        continue
    if not isinstance(wk, (bytes, bytearray)) or len(wk) == 0:
        continue
    if not seq:
        continue
    if name in seen:
        continue
    seen.add(name)
    models.append({
        "name": name,
        "read_key": [int(rk[0]) & 0xff, int(rk[1]) & 0xff],
        "write_key": [int(b) & 0xff for b in wk],
        "reset": seq,
    })

with open(OUT, "w", encoding="utf-8") as f:
    json.dump({"models": models}, f, indent=1)

print(f"Wrote {len(models)} resettable models to {OUT}")
