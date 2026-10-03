# /// script
# requires-python = ">=3.10"
# dependencies = ["pyelftools>=0.29"]
# ///
"""Write a node's A2L with the addresses of one particular build.

Variable addresses move with every build, so the hand-written A2L carries
placeholders and names each object's variable with SYMBOL_LINK. This fills in
ECU_ADDRESS (MEASUREMENT) and the address field (CHARACTERISTIC) from the
ELF's symbol table, and the EPK and ADDR_EPK from the firmware's EPK string
(zelos_xcp_epk), so the A2L names the one build it describes.

    uv run tools/a2l_addresses.py nodes/dcdc/dcdc.a2l build/dcdc-xcp.elf build/dcdc-xcp.a2l
"""

import re
import sys

from elftools.elf.elffile import ELFFile

BLOCK = re.compile(r"/begin (MEASUREMENT|CHARACTERISTIC)\b.*?/end \1", re.S)
LINK = re.compile(r'SYMBOL_LINK\s+"([^"]+)"\s+(-?\d+)')
ECU_ADDRESS = re.compile(r"(ECU_ADDRESS\s+)0x[0-9A-Fa-f]+")
# Name, quoted long identifier, type, then the address.
CHAR_ADDRESS = re.compile(r'(/begin CHARACTERISTIC\s+\S+\s+"[^"]*"\s+\S+\s+)0x[0-9A-Fa-f]+')
EPK = re.compile(r'(\bEPK\s+)"[^"]*"')
ADDR_EPK = re.compile(r"(\bADDR_EPK\s+)0x[0-9A-Fa-f]+")
EPK_SYMBOL = "zelos_xcp_epk"


def symbols(elf_path):
    with open(elf_path, "rb") as f:
        symtab = ELFFile(f).get_section_by_name(".symtab")
        if symtab is None:
            sys.exit(f"{elf_path}: no symbol table")
        return {
            s.name: (s["st_value"], s["st_size"])
            for s in symtab.iter_symbols()
            if s["st_info"]["type"] == "STT_OBJECT"
        }


def epk(elf_path, syms):
    """The EPK string's address and text, read from the ELF's loaded image."""
    if EPK_SYMBOL not in syms:
        sys.exit(f"{EPK_SYMBOL} is not in the ELF: is this the XCP build?")
    address, size = syms[EPK_SYMBOL]
    with open(elf_path, "rb") as f:
        for section in ELFFile(f).iter_sections():
            start = section["sh_addr"]
            if section["sh_type"] == "SHT_PROGBITS" and start <= address < start + section["sh_size"]:
                raw = section.data()[address - start : address - start + size]
                return address, raw.rstrip(b"\0").decode("ascii")
    sys.exit(f"{EPK_SYMBOL} at 0x{address:08X} is in no loaded section")


def patch_epk(a2l, address, text):
    """EPK and ADDR_EPK in MOD_PAR: exactly one of each, or the template is wrong."""
    a2l, n = EPK.subn(lambda m: f'{m.group(1)}"{text}"', a2l)
    a2l, m = ADDR_EPK.subn(lambda m: f"{m.group(1)}0x{address:08X}", a2l)
    if (n, m) != (1, 1):
        sys.exit(f"expected one EPK and one ADDR_EPK in the template, found {n} and {m}")
    return a2l


def patch(a2l, syms):
    def one(match):
        block = match.group(0)
        link = LINK.search(block)
        if link is None:
            sys.exit(f"no SYMBOL_LINK in:\n{block}")
        name, offset = link.group(1), int(link.group(2))
        if name not in syms:
            sys.exit(f"symbol {name} is not in the ELF: is this the XCP build?")
        value, size = syms[name]
        # A wrong address reads or calibrates the wrong variable, so be strict.
        if not 0 <= offset < size:
            sys.exit(f"{name}: offset {offset} outside its {size} bytes")
        address = f"0x{value + offset:08X}"
        pattern = ECU_ADDRESS if match.group(1) == "MEASUREMENT" else CHAR_ADDRESS
        block, n = pattern.subn(lambda m: m.group(1) + address, block, count=1)
        if n != 1:
            sys.exit(f"{name}: no address field to fill")
        return block

    return BLOCK.sub(one, a2l)


def main():
    if len(sys.argv) != 4:
        sys.exit(f"usage: {sys.argv[0]} TEMPLATE.a2l BUILD.elf OUT.a2l")
    template, elf, out = sys.argv[1:]
    with open(template) as f:
        a2l = f.read()
    syms = symbols(elf)
    a2l = patch(a2l, syms)
    if ADDR_EPK.search(a2l):
        a2l = patch_epk(a2l, *epk(elf, syms))
    with open(out, "w") as f:
        f.write(a2l)


if __name__ == "__main__":
    main()
