from __future__ import annotations

import ast
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PY_SOURCE = ROOT / "python-workflow" / "midi2nbs.py"
CPP_SOURCE = ROOT / "cpp" / "src" / "tables.cpp"


def _literal(name: str):
    tree = ast.parse(PY_SOURCE.read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name) and node.target.id == name:
            return ast.literal_eval(node.value)
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == name for target in node.targets
        ):
            return ast.literal_eval(node.value)
    raise AssertionError(f"{name} not found in {PY_SOURCE}")


def _cpp_block(start: str) -> str:
    text = CPP_SOURCE.read_text(encoding="utf-8")
    begin = text.index(start)
    end = text.index("};", begin)
    return text[begin:end]


def test_instrument_table_matches_python() -> None:
    python_table = _literal("GM_INSTRUMENT_MAPPINGS")
    block = _cpp_block("const GmEntry kInstrumentMap[128] = {")
    cpp = re.findall(r'\{\s*"([^"]+)"\s*,\s*([+-]?\d+)\s*\}', block)
    assert len(cpp) == 128
    assert [(name, int(octave)) for name, octave in cpp] == [python_table[i] for i in range(128)]


def test_percussion_table_matches_python() -> None:
    python_table = _literal("GM_PERCUSSION_MAPPINGS")
    block = _cpp_block("const std::map<int, std::pair<const char*, int>> kPercussionMap = {")
    cpp = {
        int(gm): (name, int(key))
        for gm, name, key in re.findall(r'\{\s*(\d+),\s*\{\s*"([^"]+)"\s*,\s*([+-]?\d+)\s*\}\s*\}', block)
    }
    assert cpp == python_table


def test_slot_and_drum_name_tables_match_python() -> None:
    block = _cpp_block("const std::vector<std::pair<int, const char*>> kGmToSlotOrdered = {")
    cpp_ordered = dict((int(gm), slot) for gm, slot in re.findall(r'\{\s*(\d+),\s*"([^"]+)"\s*\}', block))
    assert list(cpp_ordered.items()) == list(_literal("GM_TO_SLOT").items())

    block = _cpp_block("const std::map<int, const char*> kGmToSlot = {")
    cpp_map = {int(gm): slot for gm, slot in re.findall(r'\{\s*(\d+),\s*"([^"]+)"\s*\}', block)}
    assert cpp_map == _literal("GM_TO_SLOT")

    block = _cpp_block("const std::vector<std::pair<int, const char*>> kGmDrumNames = {")
    cpp_names = dict((int(gm), name) for gm, name in re.findall(r'\{\s*(\d+),\s*"([^"]+)"\s*\}', block))
    assert list(cpp_names.items()) == list(_literal("GM_DRUM_NAMES").items())
