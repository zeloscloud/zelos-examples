"""The bench's AUTOSAR system description, arxml/can-full.arxml, as a tester reads it.

cantools decodes frames, PDUs, containers, multiplexed and secured PDUs and
checks E2E profiles 2 and 5. The rest a tester takes from the XML: the CanTp
addressing UDS runs on, E2E profile 1's configuration and CRC.
"""

import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

import cantools
from cryptography.hazmat.primitives.ciphers import algorithms
from cryptography.hazmat.primitives.cmac import CMAC

# testing/src/zelos_testing/ -> the repository root. A bench's tester mounts
# arxml/ at the same place relative to its copy of this package.
PATH = Path(__file__).resolve().parents[3] / "arxml" / "can-full.arxml"
NS = "{http://autosar.org/schema/r4.0}"

# The DC-DC's SecOC key: RFC 4493's AES-128 example key, public on purpose (a demo key).
SECOC_KEY = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")


def load() -> cantools.database.Database:
    return cantools.database.load_file(PATH, strict=True)


class Arxml:
    """Elements by absolute SHORT-NAME path."""

    def __init__(self, path: Path = PATH):
        self.root = ET.parse(path).getroot()
        self.by_path: dict[str, ET.Element] = {}
        self._index(self.root, "")

    def _index(self, elem, path):
        name = elem.find(f"{NS}SHORT-NAME")
        if name is not None:
            path = f"{path}/{name.text}"
            self.by_path[path] = elem
        for child in elem:
            self._index(child, path)

    def deref(self, elem, tag: str):
        return self.by_path[elem.find(f"{NS}{tag}").text]

    def all(self, tag: str):
        return self.root.iter(f"{NS}{tag}")

    def can_id(self, pdu_path: str) -> int:
        """The identifier of the frame carrying a PDU."""
        frame = next(f for f in self.all("CAN-FRAME") if f.findtext(f".//{NS}PDU-REF") == pdu_path)
        frame_path = next(p for p, e in self.by_path.items() if e is frame)
        trig = next(t for t in self.all("CAN-FRAME-TRIGGERING") if t.findtext(f"{NS}FRAME-REF") == frame_path)
        return int(trig.findtext(f"{NS}IDENTIFIER"), 0)


@dataclass(frozen=True)
class CanTp:
    """ISO-TP addressing and parameters of the diagnostic connections (CAN-TP-CONFIG)."""

    physical: int  # requests to the ECU
    functional: int  # requests to every ECU
    response: int  # the ECU's responses
    padding: bool
    block_size: int
    st_min: float  # s, the ECU's


def cantp(ecu: str = "DCDC", ax: Arxml | None = None) -> CanTp:
    """The diagnostic connections (the description has one diagnostic server), and ecu's TP node."""
    ax = ax or Arxml()
    ids, padding, block = {}, set(), set()
    for conn in ax.all("CAN-TP-CONNECTION"):
        sdu = ax.deref(conn, "TP-SDU-REF")
        kind = (sdu.findtext(f"{NS}DIAG-PDU-TYPE"), conn.findtext(f"{NS}TA-TYPE"))
        ids[kind] = ax.can_id(conn.findtext(f"{NS}DATA-PDU-REF"))
        padding.add(conn.findtext(f"{NS}PADDING-ACTIVATION") == "true")
        block.add(int(conn.findtext(f"{NS}MAX-BLOCK-SIZE")))
    connectors = {p for p, e in ax.by_path.items() if p.startswith(f"/Ecus/{ecu}/") and e.tag == f"{NS}CAN-COMMUNICATION-CONNECTOR"}
    node = next(n for n in ax.all("CAN-TP-NODE") if n.findtext(f"{NS}CONNECTOR-REF") in connectors)
    assert len(padding) == 1 and len(block) == 1, "one padding and block size per channel"
    return CanTp(
        physical=ids[("DIAG-REQUEST", "PHYSICAL")],
        functional=ids[("DIAG-REQUEST", "FUNCTIONAL")],
        response=ids[("DIAG-RESPONSE", "PHYSICAL")],
        padding=padding.pop(),
        block_size=block.pop(),
        st_min=float(node.findtext(f"{NS}ST-MIN")),
    )


@dataclass(frozen=True)
class E2eP01:
    data_id: int
    mode: str  # ALL-16-BIT, ALTERNATING-8-BIT, LOWER-8-BIT, LOWER-12-BIT
    counter_offset: int
    crc_offset: int
    data_length: int  # bits


def e2e_p01(pdu: str, ax: Arxml | None = None) -> E2eP01:
    """A PDU's profile 1 configuration: its signal group's E2E props and transformer."""
    ax = ax or Arxml()
    group = ax.deref(next(m for m in ax.by_path[f"/Pdus/{pdu}"].iter(f"{NS}I-SIGNAL-TO-I-PDU-MAPPING") if m.find(f"{NS}I-SIGNAL-GROUP-REF") is not None), "I-SIGNAL-GROUP-REF")
    props = group.find(f".//{NS}END-TO-END-TRANSFORMATION-I-SIGNAL-PROPS-CONDITIONAL")
    desc = ax.deref(props, "TRANSFORMER-REF").find(f".//{NS}END-TO-END-TRANSFORMATION-DESCRIPTION")
    assert desc.findtext(f"{NS}PROFILE-NAME") == "PROFILE_01"
    return E2eP01(
        data_id=int(props.findtext(f".//{NS}DATA-ID"), 0),
        mode=desc.findtext(f"{NS}DATA-ID-MODE"),
        counter_offset=int(desc.findtext(f"{NS}COUNTER-OFFSET")),
        crc_offset=int(desc.findtext(f"{NS}CRC-OFFSET")),
        data_length=int(props.findtext(f"{NS}DATA-LENGTH")),
    )


def _crc8_0x1d(data: bytes, crc: int) -> int:
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1D) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def p01_crc(cfg: E2eP01, data: bytes) -> int:
    """E2E profile 1's CRC of received data: CRC-8 0x1D, start and XOR 0x00 (PRS_E2E_00070),
    over the Data ID as the mode says, then every byte but the CRC (PRS_E2E_00082, 00640)."""
    counter = (data[cfg.counter_offset // 8] >> (cfg.counter_offset % 8)) & 0x0F
    lo, hi = cfg.data_id & 0xFF, cfg.data_id >> 8
    ids = {"ALL-16-BIT": [lo, hi], "ALTERNATING-8-BIT": [hi if counter % 2 else lo], "LOWER-8-BIT": [lo], "LOWER-12-BIT": [lo, 0]}[cfg.mode]
    at = cfg.crc_offset // 8
    crc = _crc8_0x1d(bytes(ids), 0x00)
    crc = _crc8_0x1d(data[:at], crc)
    return _crc8_0x1d(data[at + 1 : cfg.data_length // 8], crc)


def cmac(key: bytes):
    """A cantools SecOC authenticator: AES-128 CMAC (SecOC profile 1) under key."""

    def authenticate(dbmsg, auth_data: bytes, freshness: int) -> bytes:
        mac = CMAC(algorithms.AES(key))
        mac.update(auth_data)
        return mac.finalize()

    return authenticate
