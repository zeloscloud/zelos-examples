# /// script
# requires-python = ">=3.12"
# dependencies = ["cantools==41.3.1"]
# [tool.uv]
# exclude-newer = "2026-10-01T00:00:00Z"
# ///
"""C tables for one ECU's AUTOSAR I-PDUs, from an AUTOSAR System Template.

    uv run tools/arxml2c.py arxml/can-full.arxml DCDC nodes/dcdc/src/autosar_pdus

writes autosar_pdus.h and autosar_pdus.c (see include/zelos/autosar.h): every
frame the ECU sends whose PDU is AUTOSAR-specific (E2E protected, container,
multiplexed or secured), with its CAN id, timing, signal layouts and scaling,
E2E and SecOC parameters. Plain ISignalIPdus are the DBC codec's.

cantools reads the database; the few things it does not carry (E2E offsets and
data id mode, TIME-OFFSET, multiplexed segments) are read from the XML. Any
description the core cannot send (big-endian signals, long container headers,
unaligned SecOC fields, ...) fails here, not on the node.
"""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import cantools

NS = "{http://autosar.org/schema/r4.0}"
P01_MODES = {
    "ALL-16-BIT": "ZELOS_E2E_P01_DATAID_BOTH",
    "ALTERNATING-8-BIT": "ZELOS_E2E_P01_DATAID_ALT",
    "LOWER-8-BIT": "ZELOS_E2E_P01_DATAID_LOW",
    "LOWER-12-BIT": "ZELOS_E2E_P01_DATAID_NIBBLE",
}


class DescriptionError(Exception):
    pass


def require(ok: bool, message: str):
    if not ok:
        raise DescriptionError(message)


def snake(name: str) -> str:
    return re.sub(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", "_", name).lower()


def ident(name: str) -> str:
    return re.sub(r"\W", "_", snake(name)).upper()


class Arxml:
    """Elements by absolute SHORT-NAME path, and reference following."""

    def __init__(self, path: Path):
        self.root = ET.parse(path).getroot()
        self.by_path: dict[str, ET.Element] = {}
        self._index(self.root, "")

    def _index(self, elem: ET.Element, path: str):
        name = elem.find(f"{NS}SHORT-NAME")
        if name is not None:
            path = f"{path}/{name.text}"
            self.by_path[path] = elem
        for child in elem:
            self._index(child, path)

    def deref(self, elem: ET.Element, tag: str) -> ET.Element:
        ref = elem.find(f"{NS}{tag}")
        require(ref is not None and ref.text in self.by_path, f"unresolved {tag} under {elem.tag}")
        return self.by_path[ref.text]

    def sent_by(self, ecu: str) -> list[int]:
        """CAN ids of the frames ecu sends: triggerings with one of its frame ports OUT."""
        found = [p for p, e in self.by_path.items() if e.tag == f"{NS}ECU-INSTANCE" and p.rsplit("/", 1)[1] == ecu]
        require(len(found) == 1, f"ECU-INSTANCE {ecu}: {len(found)} found")
        out = (p for p, e in self.by_path.items() if e.tag == f"{NS}FRAME-PORT" and text(e, "COMMUNICATION-DIRECTION") == "OUT")
        ports = {p for p in out if p.startswith(f"{found[0]}/")}
        return sorted(
            int(text(t, "IDENTIFIER"), 0)
            for t in self.root.iter(f"{NS}CAN-FRAME-TRIGGERING")
            if {r.text for r in t.iter(f"{NS}FRAME-PORT-REF")} & ports
        )

    def pdu(self, name: str) -> ET.Element:
        found = [e for p, e in self.by_path.items() if p.rsplit("/", 1)[1] == name and e.tag.endswith("PDU")]
        require(len(found) == 1, f"PDU {name}: {len(found)} found")
        return found[0]


def text(elem: ET.Element, path: str) -> str | None:
    found = elem.find("/".join(f"{NS}{p}" for p in path.split("/")))
    return None if found is None else found.text


def number(elem: ET.Element, path: str) -> int | None:
    t = text(elem, path)
    return None if t is None else int(t, 0)


def timing(pdu: ET.Element) -> tuple[int, int]:
    """CYCLIC-TIMING period and offset in ms."""
    cyc = "I-PDU-TIMING-SPECIFICATIONS/I-PDU-TIMING/TRANSMISSION-MODE-DECLARATION/TRANSMISSION-MODE-TRUE-TIMING/CYCLIC-TIMING"
    period = text(pdu, f"{cyc}/TIME-PERIOD/VALUE")
    name = text(pdu, "SHORT-NAME")
    require(period is not None, f"{name}: no cyclic TIME-PERIOD to send it on")
    offset = text(pdu, f"{cyc}/TIME-OFFSET/VALUE") or "0"
    period_ms, offset_ms = float(period) * 1000, float(offset) * 1000
    require(period_ms == round(period_ms) and offset_ms == round(offset_ms), f"{name}: timing not in whole ms")
    return round(period_ms), round(offset_ms)


def comment(*parts: str | None) -> str:
    return "/* " + " ".join(p for p in parts if p) + " */"


def segments(part: ET.Element) -> list[tuple[int, int]]:
    out = []
    for s in part.iter(f"{NS}SEGMENT-POSITION"):
        if s.find(f"{NS}SEGMENT-LENGTH") is None:
            continue  # the SEGMENT-POSITION child of a SEGMENT-POSITION
        require(text(s, "SEGMENT-BYTE-ORDER") in (None, "MOST-SIGNIFICANT-BYTE-LAST"), "big-endian segment")
        out.append((number(s, "SEGMENT-POSITION"), number(s, "SEGMENT-LENGTH")))
    return out


class Writer:
    def __init__(self):
        self.h: list[str] = []
        self.c: list[str] = []
        self.tx: list[str] = []

    def pdu(self, name: str, elem: ET.Element):
        length = number(elem, "LENGTH")
        unused = number(elem, "UNUSED-BIT-PATTERN")
        unused = 0xFF if unused is None else unused
        self.h.append(f"extern const struct zelos_ar_pdu ar_{snake(name)};")
        self.c.append(f"const struct zelos_ar_pdu ar_{snake(name)} = {{.length = {length}, .unused = 0x{unused:02X}}};")

    def signal(self, pdu: str, s):
        require(s.byte_order == "little_endian", f"{pdu}.{s.name}: big-endian")
        require(1 <= s.length <= 32, f"{pdu}.{s.name}: {s.length} bits")
        full = (-(1 << (s.length - 1)), (1 << (s.length - 1)) - 1) if s.is_signed else (0, (1 << s.length) - 1)
        lo, hi = full
        if s.minimum is not None and s.maximum is not None:
            lo = round((s.minimum - s.offset) / s.scale)
            hi = round((s.maximum - s.offset) / s.scale)
        # The core scales in float, exact for integers up to 2^24.
        require(full[0] <= lo <= hi <= full[1] and -(2**24) <= lo and hi <= 2**24, f"{pdu}.{s.name}: range")
        invalid = [raw for raw in (s.choices or {}) if not lo <= raw <= hi]
        has_linear = s.minimum is not None
        require(len(invalid) <= 1 or not has_linear, f"{pdu}.{s.name}: several values beyond the scale")
        fields = [
            f".start = {s.start}",
            f".length = {s.length}",
            f".factor = {float(s.scale)!r}f",
            f".offset = {float(s.offset)!r}f",
            f".min = {lo}",
            f".max = {hi}",
        ]
        if has_linear and invalid:
            fields += [".has_invalid = true", f".invalid = 0x{invalid[0]:X}"]
        cname = f"ar_{snake(pdu)}_{snake(s.name)}"
        self.h.append(f"extern const struct zelos_ar_signal {cname};")
        self.c.append(f"const struct zelos_ar_signal {cname} = {{{', '.join(fields)}}};")
        for raw, choice in sorted((s.choices or {}).items()):
            self.h.append(f"#define {ident(f'AR_{pdu}_{s.name}_{choice}')} {raw}")

    def define(self, name: str, value):
        self.h.append(f"#define {name} {value}")

    def scheduled(self, name: str, can_id: int, elem: ET.Element):
        period, offset = timing(elem)
        up = ident(f"AR_{name}")
        self.define(f"{up}_ID", f"0x{can_id:03X}")
        self.define(f"{up}_PERIOD_MS", period)
        self.define(f"{up}_OFFSET_MS", offset)
        self.tx.append(f"X({snake(name)}, {ident(name)})")


def e2e(ax: Arxml, pdu_elem: ET.Element, name: str, w: Writer, length: int):
    groups = [m for m in pdu_elem.iter(f"{NS}I-SIGNAL-TO-I-PDU-MAPPING") if m.find(f"{NS}I-SIGNAL-GROUP-REF") is not None]
    require(len(groups) == 1, f"{name}: {len(groups)} signal groups")
    group = ax.deref(groups[0], "I-SIGNAL-GROUP-REF")
    props = group.find(f".//{NS}END-TO-END-TRANSFORMATION-I-SIGNAL-PROPS-CONDITIONAL")
    require(props is not None, f"{name}: no E2E transformation props")
    tech = ax.deref(props, "TRANSFORMER-REF")
    d = tech.find(f".//{NS}END-TO-END-TRANSFORMATION-DESCRIPTION")
    profile = text(d, "PROFILE-NAME")
    ids = [int(x.text, 0) for x in props.iter(f"{NS}DATA-ID")]
    bits = number(props, "DATA-LENGTH")
    require(bits is not None and bits % 8 == 0 and bits <= length * 8, f"{name}: DATA-LENGTH {bits}")
    cname = f"ar_{snake(name)}_e2e"
    if profile == "PROFILE_01":
        mode = text(d, "DATA-ID-MODE")
        require(mode in P01_MODES and len(ids) == 1, f"{name}: P01 data id / mode")
        counter, crc = number(d, "COUNTER-OFFSET"), number(d, "CRC-OFFSET")
        nibble = number(d, "DATA-ID-NIBBLE-OFFSET") or 0
        require(counter % 4 == 0 and crc % 8 == 0 and nibble % 4 == 0, f"{name}: P01 offsets")
        require(max(counter + 4, crc + 8, nibble + 4) <= bits, f"{name}: P01 offsets beyond DATA-LENGTH")
        w.h.append(f"extern const struct zelos_e2e_p01_config {cname};")
        w.c.append(
            f"const struct zelos_e2e_p01_config {cname} = {{.data_id = 0x{ids[0]:04X}, "
            f".data_id_mode = {P01_MODES[mode]}, .counter_offset = {counter}, .crc_offset = {crc}, "
            f".data_id_nibble_offset = {nibble}, .data_length = {bits}}};"
        )
    elif profile == "PROFILE_02":
        require(len(ids) == 16 and all(0 <= i < 256 for i in ids), f"{name}: P02 needs 16 8-bit data ids")
        require((number(d, "OFFSET") or 0) == 0, f"{name}: P02 has a fixed layout, OFFSET 0")
        w.h.append(f"extern const struct zelos_e2e_p02_config {cname};")
        w.c.append(
            f"const struct zelos_e2e_p02_config {cname} = {{.data_ids = {{"
            + ", ".join(f"0x{i:02X}" for i in ids)
            + f"}}, .data_length = {bits}}};"
        )
    elif profile == "PROFILE_05":
        offset = number(d, "OFFSET") or 0
        require(len(ids) == 1 and offset % 8 == 0 and offset + 24 <= bits, f"{name}: P05 data id / offset")
        w.h.append(f"extern const struct zelos_e2e_p05_config {cname};")
        w.c.append(
            f"const struct zelos_e2e_p05_config {cname} = {{.data_id = 0x{ids[0]:04X}, "
            f".offset = {offset}, .data_length = {bits}}};"
        )
    else:
        raise DescriptionError(f"{name}: E2E {profile} not implemented")


def generate(arxml: Path, ecu: str) -> tuple[str, str]:
    db = cantools.database.load_file(arxml, strict=True)
    ax = Arxml(arxml)
    system = next(e for e in ax.by_path.values() if e.tag == f"{NS}SYSTEM")
    w = Writer()
    for can_id in ax.sent_by(ecu):
        m = db.get_message_by_frame_id(can_id)
        require(not m.is_extended_frame and not m.is_fd and m.length <= 8, f"{m.name}: classic 11-bit frames only")
        elem = ax.by_path[m.autosar.pdu_paths[0]]
        kind = elem.tag.removeprefix(NS)
        if kind not in ("CONTAINER-I-PDU", "MULTIPLEXED-I-PDU", "SECURED-I-PDU") and not m.autosar.e2e:
            continue  # a plain ISignalIPdu: the DBC codec's
        name = text(elem, "SHORT-NAME")
        w.h += ["", comment(f"{name}: frame 0x{m.frame_id:03X}, {kind}.", m.comment)]
        up = ident(f"AR_{name}")
        if kind == "I-SIGNAL-I-PDU":
            w.pdu(name, elem)
            w.scheduled(name, m.frame_id, elem)
            for s in m.signals:
                w.signal(name, s)
            e2e(ax, elem, name, w, m.length)
        elif kind == "CONTAINER-I-PDU":
            require(text(elem, "HEADER-TYPE") == "SHORT-HEADER", f"{name}: short headers only")
            require(text(system, "CONTAINER-I-PDU-HEADER-BYTE-ORDER") == "MOST-SIGNIFICANT-BYTE-FIRST",
                    "container headers most significant byte first only")
            w.define(f"{up}_ID", f"0x{m.frame_id:03X}")
            for c in m.contained_messages:
                require(4 + c.length <= m.length, f"{c.name}: does not fit {name} with its header")
                celem = ax.pdu(c.name)
                w.h.append(comment(f"{c.name}: contained in {name}.", c.comment))
                w.pdu(c.name, celem)
                w.scheduled(c.name, m.frame_id, celem)
                w.define(f"{ident(f'AR_{c.name}')}_HEADER_ID", f"0x{c.header_id:06X}")
                for s in c.signals:
                    w.signal(c.name, s)
        elif kind == "MULTIPLEXED-I-PDU":
            w.define(f"{up}_ID", f"0x{m.frame_id:03X}")
            w.pdu(name, elem)
            static = elem.find(f"{NS}STATIC-PARTS/{NS}STATIC-PART")
            static_name = text(ax.deref(static, "I-PDU-REF"), "SHORT-NAME")
            dynamic = elem.find(f"{NS}DYNAMIC-PARTS/{NS}DYNAMIC-PART")
            for part, segs in ((f"{name}_static", segments(static)), (f"{name}_dynamic", segments(dynamic))):
                w.h.append(f"extern const struct zelos_ar_segment ar_{snake(part)}_segments[{len(segs)}];")
                w.c.append(f"const struct zelos_ar_segment ar_{snake(part)}_segments[{len(segs)}] = {{"
                           + ", ".join(f"{{{a}, {b}}}" for a, b in segs) + "};")
            w.h.append(f"/* {static_name}: static part of {name}. */")
            w.pdu(static_name, ax.pdu(static_name))
            muxer = next(s for s in m.signals if s.is_multiplexer)
            require((muxer.start, muxer.length) == (number(elem, "SELECTOR-FIELD-START-POSITION"),
                                                    number(elem, "SELECTOR-FIELD-LENGTH")), f"{name}: selector")
            muxer.name, muxer.choices = "Selector", None  # cantools names it {frame}_selector1
            w.signal(name, muxer)
            for s in m.signals:
                if s.multiplexer_signal is None and not s.is_multiplexer:
                    w.signal(static_name, s)
            for alt in dynamic.iter(f"{NS}DYNAMIC-PART-ALTERNATIVE"):
                aelem = ax.deref(alt, "I-PDU-REF")
                aname = text(aelem, "SHORT-NAME")
                code = number(alt, "SELECTOR-FIELD-CODE")
                w.h.append(f"/* {aname}: dynamic part of {name}, selector {code}. */")
                w.pdu(aname, aelem)
                w.scheduled(aname, m.frame_id, aelem)
                w.define(f"{ident(f'AR_{aname}')}_SELECTOR", code)
                for s in m.signals:
                    if s.multiplexer_ids and code in s.multiplexer_ids:
                        w.signal(aname, s)
        elif kind == "SECURED-I-PDU":
            sec = m.autosar.secoc
            auth = ax.deref(elem, "AUTHENTICATION-PROPS-REF")
            require(text(auth, "AUTH-ALGORITHM") == "CMAC/AES-128", f"{name}: CMAC/AES-128 only")
            require(sec.freshness_bit_length % 8 == 0 and sec.freshness_bit_length <= 32
                    and sec.freshness_tx_bit_length % 8 == 0 and sec.auth_tx_bit_length % 8 == 0
                    and sec.auth_tx_bit_length <= 128, f"{name}: byte-aligned SecOC fields only")
            require(sec.payload_length + (sec.freshness_tx_bit_length + sec.auth_tx_bit_length) // 8 == m.length,
                    f"{name}: payload and trailer do not fill the PDU")
            payload = ax.deref(ax.deref(elem, "PAYLOAD-REF"), "I-PDU-REF")
            pname = text(payload, "SHORT-NAME")
            w.define(f"{up}_LENGTH", m.length)
            w.define(f"{up}_DATA_ID", f"0x{sec.data_id:04X}")
            w.define(f"{up}_FRESHNESS_BITS", sec.freshness_bit_length)
            w.define(f"{up}_FRESHNESS_TX_BITS", sec.freshness_tx_bit_length)
            w.define(f"{up}_AUTH_TX_BITS", sec.auth_tx_bit_length)
            w.h.append(f"/* {pname}: the authentic I-PDU of {name}. */")
            w.pdu(pname, payload)
            w.scheduled(pname, m.frame_id, payload)
            trailer = {f"{m.name}_Freshness", f"{m.name}_Authenticator"}  # cantools' pseudo-signals
            for s in m.signals:
                if s.name not in trailer:
                    w.signal(pname, s)
    require(w.tx, f"{ecu} sends no AUTOSAR-specific PDU")

    source = arxml.as_posix()
    head = [
        "/*",
        f" * {ecu}'s AUTOSAR I-PDUs: generated by tools/arxml2c.py from {source}.",
        " * Do not edit: change the ARXML and run `just arxml`. See include/zelos/autosar.h.",
        " */",
    ]
    guard = "AUTOSAR_PDUS_H_"
    h = head + ["", f"#ifndef {guard}", f"#define {guard}", "", "#include <zelos/autosar.h>", "",
                f"/* Every I-PDU {ecu} sends on its own timing, as X(name, NAME). */",
                "#define AR_TX_PDUS(X) \\"] + [f"\t{x} \\" for x in w.tx[:-1]] + [f"\t{w.tx[-1]}"]
    h += w.h + ["", f"#endif /* {guard} */", ""]
    c = head + ["", '#include "autosar_pdus.h"', ""] + w.c + [""]
    return "\n".join(h), "\n".join(c)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.split("\n\n")[1])
    arxml, ecu, out = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
    try:
        h, c = generate(arxml, ecu)
    except DescriptionError as e:
        sys.exit(f"{arxml}: {e}")
    out.with_suffix(".h").write_text(h)
    out.with_suffix(".c").write_text(c)
    print(f"{out}.h, {out}.c generated from {arxml}")


if __name__ == "__main__":
    main()
