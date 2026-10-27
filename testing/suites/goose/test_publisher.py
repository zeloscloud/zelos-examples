"""The combined image's GOOSE (lib/goose, nodes/full/src/goose.c), captured raw
on the host interface on the node's subnet, the PDU decoded by a BER walk
written from IEC 61850-8-1 Annex A.

    cd testing && sudo uv run pytest suites/goose --channel can0

Capturing needs CAP_NET_RAW: the bench's tester has it, a user shell does not.
"""

import itertools
import socket
import struct
import time

import pytest

from zelos_testing import frames
from zelos_testing.bench_plugin import eth_link

ETHERTYPE = 0x88B8
# APPID: (gocbRef, datSet, goID, numDatSetEntries, period ms or None for on change).
CBS = {
    0x0001: ("ZELOSFULL/LLN0$GO$gcb_pdu", "ZELOSFULL/LLN0$ds_pdu", "zelos-pdu", 17, None),
    0x0002: ("ZELOSFULL/LLN0$GO$gcb_dcdc", "ZELOSFULL/LLN0$ds_dcdc", "zelos-dcdc", 5, 100),
}
DST = {0x0001: bytes.fromhex("010ccd010001"), 0x0002: bytes.fromhex("010ccd010002")}
# On change: twice the time to the next frame, 4 ms doubling to 512, then 1000.
TAL_ON_CHANGE = {2 * 4 << k for k in range(8)} | {2000}
CAPTURE_S = 6.0
# Link up after a boot, before the first frame.
UP_S = 10.0
# Gaps between gcb_dcdc frames, on hardware.
PERIOD_TOL = 0.05
# Linux's, which Python's socket module does not export: a struct timespec per frame.
SO_TIMESTAMPNS = 35


def tlv(b: bytes, i: int) -> tuple[int, int, int]:
    """(tag, content start, content end) of the TLV at b[i]: one-byte tags, definite lengths."""
    tag, n = b[i], b[i + 1]
    if n < 0x80:
        return tag, i + 2, i + 2 + n
    width = n & 0x7F
    n = int.from_bytes(b[i + 2 : i + 2 + width])
    return tag, i + 2 + width, i + 2 + width + n


def parse(frame: bytes) -> dict:
    """The header and the IECGoosePdu's fields, by name. Asserts on structure."""
    dst, src, ethertype, appid, length, res1, res2 = struct.unpack_from("!6s6sHHHHH", frame)
    assert ethertype == ETHERTYPE and (res1, res2) == (0, 0)
    tag, start, end = tlv(frame, 22)
    assert tag == 0x61 and end == 14 + length <= len(frame), (tag, end, length, len(frame))
    names = ["gocbRef", "timeAllowedtoLive", "datSet", "goID", "t", "stNum", "sqNum", "simulation", "confRev", "ndsCom", "numDatSetEntries", "allData"]
    pdu, i = {}, start
    while i < end:
        tag, a, b = tlv(frame, i)
        k = tag & 0x1F
        v = frame[a:b]
        pdu[names[k]] = v.decode() if k in (0, 2, 3) else int.from_bytes(v, signed=True) if k in (1, 5, 6, 8, 10) else v
        i = b
    assert i == end and [n for n in names if n in pdu] == names, sorted(pdu)
    members, j = 0, 0
    while j < len(pdu["allData"]):
        j = tlv(pdu["allData"], j)[2]
        members += 1
    assert j == len(pdu["allData"])
    return pdu | {"dst": dst, "src": src, "appid": appid, "members": members}


def capture(iface: str) -> list[tuple[float, dict]]:
    """GOOSE frames, kernel-stamped, until gcb_dcdc has sent CAPTURE_S of its periods and gcb_pdu at least one."""
    try:
        s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETHERTYPE))
    except PermissionError:
        pytest.skip("capturing needs CAP_NET_RAW")
    want = int(CAPTURE_S * 1000 / CBS[0x0002][4]) + 1
    got: list[tuple[float, dict]] = []
    with s:
        s.bind((iface, ETHERTYPE))
        s.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPNS, 1)
        s.settimeout(1.0)
        end = time.monotonic() + frames.bound(UP_S + CAPTURE_S)
        while not (sum(p["appid"] == 2 for _, p in got) >= want and any(p["appid"] == 1 for _, p in got)):
            assert time.monotonic() < end, f"{len(got)} GOOSE frames on {iface}"
            try:
                data, anc, _, addr = s.recvmsg(1518, socket.CMSG_SPACE(16))
            except TimeoutError:
                continue
            if addr[2] == socket.PACKET_OUTGOING:
                continue
            sec, ns = struct.unpack("qq", anc[0][2]) if anc else (time.time(), 0)
            got.append((sec + ns / 1e9, parse(data)))
    return got


def test_control_blocks(pytestconfig):
    link = eth_link(pytestconfig)
    if link is None:
        pytest.skip(f"no host interface on {pytestconfig.getoption('eth_host')}'s subnet (--eth-iface)")
    got = capture(link[0])

    for appid, (ref, dat_set, go_id, count, period_ms) in CBS.items():
        seen = [(t, p) for t, p in got if p["appid"] == appid]
        assert seen, f"no APPID 0x{appid:04x}"
        for _, p in seen:
            assert p["dst"] == DST[appid]
            assert (p["gocbRef"], p["datSet"], p["goID"]) == (ref, dat_set, go_id)
            assert p["numDatSetEntries"] == p["members"] == count
            assert p["confRev"] >= 1 and len(p["t"]) == 8
            assert p["timeAllowedtoLive"] == 2 * period_ms if period_ms else p["timeAllowedtoLive"] in TAL_ON_CHANGE
        # A new state: stNum + 1 and sqNum 0. Otherwise sqNum + 1, none missed.
        for (_, a), (_, b) in itertools.pairwise(seen):
            st = (a["stNum"], a["sqNum"]), (b["stNum"], b["sqNum"])
            assert b["stNum"] >= a["stNum"] and b["sqNum"] == (0 if b["stNum"] > a["stNum"] else a["sqNum"] + 1), st
        if period_ms:
            gaps = [b[0] - a[0] for a, b in itertools.pairwise(seen)]
            frames.assert_period(gaps, period_ms / 1000, PERIOD_TOL)
