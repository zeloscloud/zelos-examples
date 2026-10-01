"""An XCP node's A2L as its build wrote it, and the DAQ packets the node sends."""

import re
from pathlib import Path

import can

# A DAQ list's first ODT carries the timestamp right after the PID: 4 bytes,
# Intel order, 1 us per tick, wrapping at 32 bits.
TIMESTAMP_TICK_S = 1e-6
_WRAP = 1 << 32


def addresses(text: str) -> dict[str, int]:
    """Object name -> address, of every MEASUREMENT and CHARACTERISTIC."""
    addrs = {}
    for kind, name, body in re.findall(r"/begin (MEASUREMENT|CHARACTERISTIC) (\S+)(.*?)/end \1", text, re.S):
        field = r"ECU_ADDRESS\s+(0x[0-9A-Fa-f]+)" if kind == "MEASUREMENT" else r'^\s*"[^"]*"\s+\S+\s+(0x[0-9A-Fa-f]+)'
        addrs[name] = int(re.search(field, body).group(1), 16)
    return addrs


def epk(text: str) -> tuple[str, int]:
    """(EPK, ADDR_EPK) from MOD_PAR: the build's identity and where the node holds it."""
    return re.search(r'\bEPK\s+"([^"]*)"', text).group(1), int(re.search(r"\bADDR_EPK\s+(0x[0-9A-Fa-f]+)", text).group(1), 16)


def read(path: Path) -> str:
    """The A2L's text; fails on placeholders, which mean the template, not the build's."""
    text = path.read_text()
    assert all(addresses(text).values()) and epk(text)[1], f"placeholder addresses in {path}: not the generated A2L?"
    return text


def odt(frames: list[can.Message], dto_id: int, pid: int) -> list[can.Message]:
    """The DTOs of one ODT, by its absolute ODT number."""
    return [m for m in frames if m.arbitration_id == dto_id and m.data[0] == pid]


def timestamp(dto: can.Message) -> int:
    """The DAQ timestamp of a sample, from its list's first ODT."""
    return int.from_bytes(dto.data[1:5], "little")


def ticks(a: int, b: int) -> int:
    """Timestamp ticks from a to b, across a wrap."""
    return (b - a) % _WRAP
