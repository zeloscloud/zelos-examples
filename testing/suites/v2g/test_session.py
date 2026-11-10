"""DC charging sessions between EVerest's EVSE and EV, ISO 15118-2 and DIN 70121,
checked on the link they crossed.

The capture is walked at the V2GTP layer (ISO 15118-2 clause 7.8): SDP on UDP
15118, then each direction of the TCP connection split into V2GTP frames with
nothing left over. EXI is not decoded here. Each frame is named by the EVSE's
own record of it (EvseV2G's v2g_messages variable), accepted only where that
record carries the frame's bytes; the EV encodes with another codec
(EXIficient, in Josev), so a session that completes is two codecs agreeing.

    just test-v2g                 # every test
    just v2g-record               # re-record captures/ from the sessions
"""

import csv
import itertools
import shutil
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest

HERE = Path(__file__).parent
V2G_PORT = 15118
SDP_REQ, SDP_RES, EXI = 0x9000, 0x9001, 0x8001
# What the EV's script does after START (conftest), and the end each test waits for.
STOP_AFTER_S = 5
EV_STOP = f"sleep {STOP_AFTER_S};iso_stop_charging;iso_wait_v2g_session_stopped;unplug"
PROTOCOLS = {"iso2": "ISO15118-2-2013", "din": "DIN70121"}

SETUP = ["SupportedAppProtocol", "SessionSetup", "ServiceDiscovery"]
CHARGE = ["CableCheck", "PreCharge", "PowerDelivery", "CurrentDemand"]
STOP = ["PowerDelivery", "WeldingDetection", "SessionStop"]


@dataclass
class Frame:
    t: float
    # The capture's frame number of the segment holding the V2GTP frame's first byte.
    number: int
    from_ev: bool
    type: int
    payload: bytes
    raw: bytes


def fields(pcap: Path, *names: str) -> list[list[str]]:
    out = subprocess.run(
        ["tshark", "-r", str(pcap), "-T", "fields", "-E", "separator=|", *itertools.chain.from_iterable(("-e", n) for n in names)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [line.split("|") for line in out.splitlines()]


def v2gtp(data: bytes) -> list[tuple[int, bytes, bytes]]:
    """(payload type, payload, frame) of each V2GTP frame in data, which they must fill exactly."""
    got, i = [], 0
    while i < len(data):
        assert len(data) - i >= 8, f"{len(data) - i} bytes left over"
        version, inverse, ptype, n = struct.unpack_from("!BBHI", data, i)
        assert (version, inverse) == (0x01, 0xFE), data[i : i + 8].hex()
        assert i + 8 + n <= len(data), f"frame of {n} bytes, {len(data) - i - 8} left"
        got.append((ptype, data[i + 8 : i + 8 + n], data[i : i + 8 + n]))
        i += 8 + n
    return got


def sdp(pcap: Path) -> list[tuple[str, int, int, bytes]]:
    """(source, source port, payload type, payload) of each SDP datagram."""
    rows = fields(pcap, "ipv6.src", "udp.srcport", "udp.dstport", "udp.payload")
    got = []
    for src, sport, dport, payload in rows:
        if V2G_PORT in (int(sport or 0), int(dport or 0)):
            ((ptype, body, _),) = v2gtp(bytes.fromhex(payload))
            got.append((src, int(sport), ptype, body))
    return got


def tcp(pcap: Path) -> tuple[tuple[str, int], list[Frame], set[str]]:
    """The one connection's server (address, port), its V2GTP frames in time order, and its flags seen."""
    rows = fields(pcap, "frame.number", "frame.time_epoch", "ipv6.src", "ipv6.dst", "tcp.srcport", "tcp.dstport", "tcp.seq_raw", "tcp.len", "tcp.flags.str", "tcp.payload")
    rows = [r[1:] + r[:1] for r in rows if r[4]]
    syn = [r for r in rows if "S" in r[7] and "A" not in r[7].replace("·", "")]
    assert len(syn) == 1, f"{len(syn)} connections"
    ev, server = (syn[0][1], int(syn[0][3])), (syn[0][2], int(syn[0][4]))
    flags = set()
    streams: dict[bool, list[tuple[float, int, bytes, int]]] = {True: [], False: []}
    for t, src, _, sport, _, seq, n, f, payload, number in rows:
        from_ev = (src, int(sport)) == ev
        assert from_ev or (src, int(sport)) == server, f"a second peer {src}:{sport}"
        flags.update(c for c in f if c.isalpha())
        if int(n):
            streams[from_ev].append((float(t), int(seq), bytes.fromhex(payload), int(number)))
    frames = []
    for from_ev, segs in streams.items():
        # A veth loses and reorders nothing: each segment follows the last.
        for (_, a, pa, _), (_, b, _, _) in itertools.pairwise(segs):
            assert b == (a + len(pa)) % 2**32, "a gap or retransmission in the stream"
        # A frame starts in the segment holding its first byte.
        starts, data = [], b""
        for t, _, p, number in segs:
            starts.append((len(data), t, number))
            data += p
        at = 0
        for ptype, body, raw in v2gtp(data):
            _, t, number = max(s for s in starts if s[0] <= at)
            frames.append(Frame(t, number, from_ev, ptype, body, raw))
            at += len(raw)
    frames.sort(key=lambda f: f.t)
    return server, frames, flags


def names(session, frames: list[Frame]) -> list[str]:
    """Each frame's message name, from the EVSE's record of the same bytes.

    A request the EVSE never read, sent as the link went, is the one frame it
    may lack: the last, named "?".
    """
    record = session.var("iso15118_charger", "v2g_messages")
    unread = len(frames) - len(record)
    assert unread == 0 or (unread == 1 and frames[-1].from_ev), f"{len(record)} recorded, {len(frames)} on the wire"
    got = []
    for f, r in zip(frames, record, strict=False):
        exi = bytes.fromhex(r["exi"])
        assert r["id"].endswith("Req") == f.from_ev, r["id"]
        if f.from_ev:
            assert exi == f.raw, f"{r['id']}: recorded {exi.hex()}, wire {f.raw.hex()}"
        else:
            # EvseV2G 2025.10 records a response in its request's frame: the
            # request's header and length, the response's EXI cut or padded to it.
            n = min(len(exi) - 8, len(f.payload))
            assert exi[8 : 8 + n] == f.payload[:n], f"{r['id']}: recorded {exi.hex()}, wire {f.raw.hex()}"
        got.append(r["id"])
    return got + ["?"] * unread


def exchanges(msgs: list[str]) -> list[str]:
    """Request/response pairs, as one name each; asserts each request is answered by its response.

    An unanswered last request is left out.
    """
    got = []
    for req, res in zip(msgs[::2], msgs[1::2], strict=False):
        assert req.endswith("Req") and res == req[:-3] + "Res", (req, res)
        got.append(req[:-3])
    return got


def runs(seq: list[str]) -> list[str]:
    return [k for k, _ in itertools.groupby(seq)]


def check_link(pcap: Path) -> tuple[list[Frame], set[str]]:
    """SDP found the EVSE that served the connection; every frame is EXI."""
    found = sdp(pcap)
    reqs = [(s, b) for s, _, t, b in found if t == SDP_REQ]
    res = [(s, b) for s, _, t, b in found if t == SDP_RES]
    assert reqs and res, found
    # Request: security, transport (0x10 no TLS, 0x00 TCP). Response: the
    # EVSE's address and port, then the same two.
    assert all(b == bytes([0x10, 0x00]) for _, b in reqs), reqs
    server, frames, flags = tcp(pcap)
    for src, b in res:
        assert len(b) == 20 and b[18:] == bytes([0x10, 0x00]), b.hex()
        assert (src, struct.unpack_from("!H", b, 16)[0]) == server
    assert all(f.type == EXI for f in frames)
    # Request, response, request, ...
    assert [f.from_ev for f in frames] == [i % 2 == 0 for i in range(len(frames))]
    return frames, flags


def write_messages(path: Path, frames: list[Frame], msgs: list[str]) -> None:
    """The capture's sidecar: each V2GTP frame's frame number, direction and message name."""
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["frame", "direction", "message"])
        w.writerows((fr.number, "ev" if fr.from_ev else "evse", m) for fr, m in zip(frames, msgs, strict=True))


def ended(s) -> bool:
    """The EV's V2G session is over, however it ended."""
    return bool(s.var("iso15118_car", "v2g_session_finished"))


def unplugged(s) -> bool:
    return any(e.get("event") == "SessionFinished" for e in s.var("evse_manager", "session_event"))


@pytest.mark.parametrize("protocol", PROTOCOLS)
def test_session(charge, protocol):
    """The EV charges STOP_AFTER_S in CurrentDemand, then ends the session itself."""
    s = charge(protocol, EV_STOP, ended)
    frames, flags = check_link(s.pcap)
    msgs = names(s, frames)
    write_messages(s.pcap.with_suffix(".messages.csv"), frames, msgs)
    seq = exchanges(msgs)
    assert s.var("iso15118_charger", "selected_protocol") == [PROTOCOLS[protocol]]
    assert runs(seq)[:3] == SETUP, seq
    assert runs(seq)[-3:] == STOP, seq
    # The DC sequence, in order, with the loop between.
    it = iter(runs(seq))
    assert all(m in it for m in CHARGE), seq
    # Some of the loop, not a single exchange: Josev sends about two a second.
    assert seq.count("CurrentDemand") >= STOP_AFTER_S, seq
    assert "F" in flags and "R" not in flags, flags
    assert s.var("iso15118_charger", "current_demand_started") and s.var("iso15118_charger", "current_demand_finished")


def test_ev_unplug_mid_current_demand(charge):
    """Unplugged in CurrentDemand: no PowerDelivery stop, no SessionStop, and the link ends."""
    s = charge("iso2", "sleep 3;unplug", unplugged)
    frames, _ = check_link(s.pcap)
    seq = runs(exchanges(names(s, frames)))
    assert seq[-1] == "CurrentDemand", seq
    assert "SessionStop" not in seq and "WeldingDetection" not in seq


def test_isolation_fault(charge):
    """The isolation monitor reads a fault: no PreCharge, so no power."""
    s = charge("iso2", EV_STOP, ended, imd={"resistance_F_Ohm": 10000})
    frames, _ = check_link(s.pcap)
    seq = runs(exchanges(names(s, frames)))
    assert "CableCheck" in seq and "PreCharge" not in seq and "CurrentDemand" not in seq, seq


@pytest.mark.parametrize("protocol", PROTOCOLS)
def test_capture_fixture(protocol):
    """The committed demo capture holds the session test_session checks: SDP, then one
    connection of whole frames, each named in its sidecar at the frame it starts in."""
    if shutil.which("tshark") is None:
        pytest.skip("needs tshark")
    frames, flags = check_link(HERE / "captures" / f"{protocol}-dc.pcapng")
    assert len(frames) >= 2 * (len(SETUP) + len(CHARGE) + len(STOP)) and "F" in flags
    with (HERE / "captures" / f"{protocol}-dc.messages.csv").open(newline="") as f:
        rows = list(csv.DictReader(f))
    assert [(int(r["frame"]), r["direction"]) for r in rows] == [(fr.number, "ev" if fr.from_ev else "evse") for fr in frames]
    seq = exchanges([r["message"] for r in rows])
    assert runs(seq)[:3] == SETUP and runs(seq)[-3:] == STOP, seq
