"""The combined image's GOOSE (lib/goose, nodes/full/src/goose.c), captured raw
on the host interface on the node's subnet, the PDU decoded by a BER walk
written from IEC 61850-8-1 Annex A; its test knobs over UDS on CAN.

Capturing needs CAP_NET_RAW, which the bench's tester has; without it the
suite skips. On Renode, from the repository root:

    just test-bench goose can-full/build/full.elf
"""

import contextlib
import itertools
import socket
import struct
import time
from collections.abc import Callable, Iterator

import pytest

from zelos_testing import frames
from zelos_testing.bench_plugin import eth_link
from zelos_testing.frames import bound
from zelos_testing.uds import DEFAULT, EXTENDED, UdsEcu

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
# A burst's gaps, kernel receive stamps on hardware.
BURST_TOL_S = 0.002
# The test knobs (include/zelos/goose.h).
ROUTINE, SIMULATION, CONF_REV = 0x6185, 0x01, 0x03
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


@contextlib.contextmanager
def goose_socket(iface: str) -> Iterator[socket.socket]:
    """A raw socket on iface receiving GOOSE, kernel-stamped. Frames queue from here on."""
    try:
        s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETHERTYPE))
    except PermissionError:
        pytest.skip("capturing needs CAP_NET_RAW")
    with s:
        s.bind((iface, ETHERTYPE))
        s.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPNS, 1)
        s.settimeout(1.0)
        yield s


def read_until(s: socket.socket, got: list[tuple[float, dict]], done: Callable[[list], bool], node_s: float) -> None:
    """Append (stamp, parsed) frames to got until done(got), within node_s of node time."""
    end = time.monotonic() + bound(node_s)
    while not done(got):
        assert time.monotonic() < end, f"not done within {node_s} s, {len(got)} GOOSE frames"
        try:
            data, anc, _, addr = s.recvmsg(1518, socket.CMSG_SPACE(16))
        except TimeoutError:
            continue
        if addr[2] == socket.PACKET_OUTGOING:
            continue
        sec, ns = struct.unpack("qq", anc[0][2]) if anc else (time.time(), 0)
        got.append((sec + ns / 1e9, parse(data)))


def capture(iface: str) -> list[tuple[float, dict]]:
    """GOOSE frames until gcb_dcdc has sent CAPTURE_S of its periods and gcb_pdu at least one."""
    want = int(CAPTURE_S * 1000 / CBS[0x0002][4]) + 1
    got: list[tuple[float, dict]] = []
    with goose_socket(iface) as s:
        read_until(s, got, lambda g: sum(p["appid"] == 2 for _, p in g) >= want and any(p["appid"] == 1 for _, p in g), UP_S + CAPTURE_S)
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
            # gcb_dcdc's members move between frames: every frame is a new state.
            assert not period_ms or b["stNum"] == a["stNum"] + 1, st
        if period_ms:
            gaps = [b[0] - a[0] for a, b in itertools.pairwise(seen)]
            frames.assert_period(gaps, period_ms / 1000, PERIOD_TOL)


def test_stimulated_change(pytestconfig):
    """confRev + 1 is a new state on gcb_pdu: at once, then 4, 8, 16 ms on. Simulation flips, then restores."""
    link = eth_link(pytestconfig)
    if link is None:
        pytest.skip(f"no host interface on {pytestconfig.getoption('eth_host')}'s subnet (--eth-iface)")
    interface = pytestconfig.getoption("interface")
    interface = "zelos-socketcan" if interface == "socketcan" else interface
    got: list[tuple[float, dict]] = []

    def pdu(rev: int) -> list[tuple[float, dict]]:
        return [(t, p) for t, p in got if p["appid"] == 1 and p["confRev"] == rev]

    with goose_socket(link[0]) as s, UdsEcu(interface, pytestconfig.getoption("channel"), timeout_s=bound(2.0)) as ecu:

        def knob(option: int) -> bytes:
            """Result: simulation, ndsCom, confRev (uint32 big-endian)."""
            return ecu.client.start_routine(ROUTINE, bytes([option])).service_data.routine_status_record

        try:
            # A gcb_pdu frame from before, to compare with.
            read_until(s, got, lambda g: any(p["appid"] == 1 for _, p in g), UP_S)
            ecu.enter(EXTENDED)
            rev = int.from_bytes(knob(CONF_REV)[2:6])
            read_until(s, got, lambda g: len(pdu(rev)) >= 4, 2.0)
            for want in (b"\x01", b"\x00"):
                assert knob(SIMULATION)[:1] == want
                n = len(got)
                read_until(s, got, lambda g, n=n, want=want: {p["appid"] for _, p in g[n:] if p["simulation"] == want} == set(CBS), 2.0)
        finally:
            with contextlib.suppress(Exception):
                ecu.client.change_session(DEFAULT)

    old = pdu(rev - 1)[-1][1]
    burst = pdu(rev)[:4]
    assert [(p["stNum"], p["sqNum"], p["timeAllowedtoLive"]) for _, p in burst] == [(old["stNum"] + 1, k, 8 << k) for k in range(4)]
    assert burst[0][1]["t"] != old["t"]
    # Host stamps under Renode only bound them (frames.assert_period).
    gaps = [b - a for (a, _), (b, _) in itertools.pairwise(burst)]
    for gap, want in zip(gaps, (0.004, 0.008, 0.016), strict=True):
        assert gap < bound(want) + frames.HOST_JITTER_S if frames.SIMULATED else abs(gap - want) < BURST_TOL_S, gaps
