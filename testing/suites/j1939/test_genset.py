"""The genset node over J1939: address claim, requests, periods and DM1 by BAM.

Frames are checked directly, and BAM is also reassembled by python-can-j1939,
an independent J1939 stack. Each test finds the node's address by requesting
Address Claimed, so none depends on a fresh boot or on an earlier test.

Waits end on frames rather than on host seconds where they can: on Renode the
host clock measures how fast the simulation runs, not the node's timing.
"""

import contextlib
import time

import can
import j1939
import pytest

from zelos_testing import dbc
from zelos_testing.frames import Periodic, bound, collect, count, until

GENSET = dbc.load("genset")
EEC1 = GENSET.get_message_by_name("EEC1")
ET1 = GENSET.get_message_by_name("ET1")
VCU_COMMAND = dbc.load("bench").get_message_by_name("VCU_Command")

PGN_REQUEST = 0xEA00
PGN_TP_DT = 0xEB00
PGN_TP_CM = 0xEC00
PGN_CLAIM = 0xEE00
PGN_EEC1 = 61444
PGN_ET1 = 65262
PGN_DM1 = 65226
TP_BAM = 32
NULL = 0xFE
GLOBAL = 0xFF
# J1939's service tool address: the host plays one.
TESTER = 0xF9

# From nodes/genset: a hot engine's DTCs (SPN, FMI) and its amber lamp.
HOT_DTCS = {(110, 16), (1569, 31)}
# This much of the node's time bounds any one wait.
WAIT_S = 10.0
# The VCU's drive cycle. See nodes/vcu.
VCU_CYCLE_S = 15.0

pytestmark = pytest.mark.usefixtures("bus_health")


def make_id(prio, pgn, sa, da=GLOBAL):
    if (pgn >> 8) & 0xFF < 240:
        pgn = (pgn & 0x3FF00) | da
    return prio << 26 | pgn << 8 | sa


def parse(msg):
    """(pgn, da, sa) of a J1939 frame."""
    pgn = (msg.arbitration_id >> 8) & 0x3FFFF
    da = GLOBAL
    if (pgn >> 8) & 0xFF < 240:
        da, pgn = pgn & 0xFF, pgn & 0x3FF00
    return pgn, da, msg.arbitration_id & 0xFF


def theirs(msg):
    """A frame from a J1939 node other than us."""
    return msg.is_extended_id and parse(msg)[2] != TESTER


def quiet_window(bus, node_s):
    """Frames from J1939 nodes other than us over node_s of the node's time.

    For a node that stays silent, which leaves no frames to count: a host
    window, at the time scale.
    """
    return [m for m in collect(bus, bound(node_s)) if theirs(m)]


def send(bus, prio, pgn, data, sa=TESTER, da=GLOBAL):
    bus.send(can.Message(arbitration_id=make_id(prio, pgn, sa, da), data=data, is_extended_id=True))


def request(bus, pgn, da=GLOBAL):
    send(bus, 6, PGN_REQUEST, pgn.to_bytes(3, "little"), da=da)


def claims(frames):
    """(timestamp, sa, NAME) of every Address Claimed."""
    return [(m.timestamp, parse(m)[2], int.from_bytes(m.data, "little")) for m in frames if parse(m)[0] == PGN_CLAIM]


def claim(bus, sa, name):
    send(bus, 6, PGN_CLAIM, name.to_bytes(8, "little"), sa=sa)


def is_from(pgn, sa):
    return lambda m: parse(m)[0] == pgn and parse(m)[2] == sa


def nth(want, n):
    """A predicate that holds on the nth frame for which want(msg) holds."""
    seen = 0

    def counted(m):
        nonlocal seen
        seen += bool(want(m))
        return seen >= n

    return counted


def gather(bus, done, node_s=WAIT_S):
    """Frames from J1939 nodes other than us, oldest first, until done(those) holds."""
    got = []

    def step(frames):
        if not theirs(frames[-1]):
            return False
        got.append(frames[-1])
        return done(got)

    until(bus, step, node_s)
    return got


def first(bus, want):
    """Frames up to and including the first for which want(msg) holds."""
    return gather(bus, lambda got: want(got[-1]))


def answered(got):
    """A claim came back, and the claimant's next two EEC1 left time for any other."""
    found = claims(got)
    if not found:
        return False
    t, sa, _ = found[0]
    return sa == NULL or len([m for m in got if m.timestamp > t and is_from(PGN_EEC1, sa)(m)]) >= 2


@pytest.fixture
def node(bus):
    """(sa, NAME) of the one J1939 node on the bus, as it answers a request."""
    request(bus, PGN_CLAIM)
    found = [(sa, name) for _, sa, name in claims(gather(bus, answered))]
    assert len(found) == 1, f"expected one claimant, got {found}"
    sa, name = found[0]
    assert sa != NULL, "the node could not claim an address"
    return sa, name


def test_claims_address_on_request(node):
    sa, name = node
    # Self-configurable range, so the NAME must say arbitrary-address capable.
    assert 128 <= sa <= 247 and name >> 63 == 1, f"sa 0x{sa:02x} NAME 0x{name:016x}"


def test_defends_address_against_higher_name(bus, node):
    sa, name = node
    claim(bus, sa, name + 1)
    first(bus, lambda m: parse(m)[0] == PGN_CLAIM and (parse(m)[2], int.from_bytes(m.data, "little")) == (sa, name))
    # Still sending from its address after defending it.
    first(bus, is_from(PGN_EEC1, sa))


def test_yields_address_to_lower_name(bus, node, pytestconfig):
    sa, name = node
    claim(bus, sa, name - 1)
    t, moved, _ = claims(first(bus, lambda m: parse(m)[0] == PGN_CLAIM and int.from_bytes(m.data, "little") == name))[-1]
    assert moved != sa, "did not yield"
    if moved == NULL:
        assert not [m for m in quiet_window(bus, 1.5) if parse(m)[0] != PGN_CLAIM], "sent after Cannot Claim Address"
        return

    later = first(bus, lambda m: parse(m)[2] == moved and parse(m)[0] != PGN_CLAIM)
    assert all(parse(m)[2] != sa for m in later if parse(m)[0] != PGN_CLAIM), "still sending from the lost address"
    # J1939-81: 250 ms from claim to first use. The node's clock runs slightly fast.
    if not pytestconfig.getoption("channel").startswith("vcan"):
        assert later[-1].timestamp - t >= 0.245, f"used after {(later[-1].timestamp - t) * 1000:.1f} ms"


def test_answers_request_for_et1(bus, node, pytestconfig):
    sa, _ = node
    et1 = is_from(PGN_ET1, sa)

    # Just after a periodic ET1, the next is a second away: a quick one is the reply.
    first(bus, et1)
    request(bus, PGN_ET1, da=sa)
    t = time.monotonic()
    frames = first(bus, et1)
    if pytestconfig.getoption("channel").startswith("vcan"):
        # In the node's time: before its next EEC1 but one, 100 ms apart.
        assert len([m for m in frames if is_from(PGN_EEC1, sa)(m)]) <= 2
    else:
        assert time.monotonic() - t < 0.2
    assert -40 <= ET1.decode(frames[-1].data)["EngineCoolantTemp"] <= 210


def test_periods(bus, node, pytestconfig):
    sa, _ = node
    # From one ET1, the slowest message, to the third after it.
    frames = first(bus, is_from(PGN_ET1, sa))[-1:]
    frames += first(bus, nth(is_from(PGN_ET1, sa), 3))

    for pgn, msg in ((PGN_EEC1, EEC1), (PGN_ET1, ET1)):
        stamps = [m.timestamp for m in frames if is_from(pgn, sa)(m)]
        period = msg.cycle_time / 1000
        gaps = [b - a for a, b in zip(stamps, stamps[1:])]
        mean = sum(gaps) / len(gaps)
        if pytestconfig.getoption("channel").startswith("vcan"):
            assert max(gaps) < 5 * mean, f"PGN {pgn} gaps {gaps}"
        else:
            # 2%: an untrimmed node clock; the host stamps on receipt.
            assert abs(mean - period) < 0.02 * period, f"PGN {pgn} mean {mean * 1000:.2f} ms"

    # On any clock the periods keep their ratio: ten EEC1 per ET1, +-1 for jitter.
    eec1 = len([m for m in frames if is_from(PGN_EEC1, sa)(m)])
    assert abs(eec1 - 3 * ET1.cycle_time / EEC1.cycle_time) <= 1, f"{eec1} EEC1 in three ET1 periods"

    speeds = [EEC1.decode(m.data)["EngineSpeed"] for m in frames if parse(m)[0] == PGN_EEC1]
    assert all(0 <= s <= 3000 for s in speeds), speeds


def hot(bus, sa, bam_dm1):
    """The first DM1 BAM announcement, within two passes of the VCU's drive cycle.

    Counted in the genset's own EEC1, so the bound is the node's time whether
    the real VCU or this suite drives it.
    """
    eec1 = is_from(PGN_EEC1, sa)
    limit = 2 * VCU_CYCLE_S * 1000 / EEC1.cycle_time
    frames = gather(bus, lambda got: bam_dm1(got[-1]) or sum(map(eec1, got)) > limit, 2 * VCU_CYCLE_S + WAIT_S)
    if not bam_dm1(frames[-1]):
        pytest.fail(f"no DM1 by BAM in {limit:.0f} EEC1")
    return frames[-1]


def test_dm1_by_bam_when_hot(bus, node, pytestconfig):
    """Drive the engine hot, then reassemble DM1.

    Plays the VCU, unless a real one is on the bus: then its drive cycle heats
    the engine.
    """
    sa, _ = node
    heartbeat = 0

    def drive():
        nonlocal heartbeat
        heartbeat = (heartbeat + 1) & 0xFF
        data = VCU_COMMAND.encode({"RequestedMode": 1, "TorqueRequest": 150.0, "Heartbeat": heartbeat})
        return can.Message(arbitration_id=VCU_COMMAND.frame_id, data=data, is_extended_id=False)

    def bam_dm1(m):
        pgn, _, src = parse(m)
        return pgn == PGN_TP_CM and src == sa and m.data[0] == TP_BAM and int.from_bytes(m.data[5:8], "little") == PGN_DM1

    with contextlib.ExitStack() as stack:
        # A second of the node's time sees ten from a VCU.
        seen = until(bus, lambda frames: sum(theirs(m) and is_from(PGN_EEC1, sa)(m) for m in frames) >= 10, WAIT_S)
        if not count(seen, VCU_COMMAND.frame_id):
            stack.enter_context(Periodic(bus, drive, VCU_COMMAND.cycle_time / 1000))
        cm = hot(bus, sa, bam_dm1)
        size, packets = int.from_bytes(cm.data[1:3], "little"), cm.data[3]
        frames = [cm] + first(bus, lambda m: parse(m)[0] == PGN_TP_DT and parse(m)[2] == sa and m.data[0] == packets)

    tp = [m for m in frames if parse(m)[2] == sa and parse(m)[0] in (PGN_TP_CM, PGN_TP_DT)]
    assert [m.data[0] for m in tp[1:]] == list(range(1, packets + 1)), [bytes(m.data).hex() for m in tp]

    # J1939-21: 50 to 200 ms between BAM packets. Renode's clock only runs slow
    # against the host's, so there only the low end holds, within 10%.
    gaps = [b.timestamp - a.timestamp for a, b in zip(tp, tp[1:])]
    if pytestconfig.getoption("channel").startswith("vcan"):
        lower, upper = 0.9 * 0.05, float("inf")
    else:
        lower, upper = 0.05, 0.2
    assert all(lower <= g <= upper for g in gaps), [round(g * 1000, 1) for g in gaps]

    data = b"".join(bytes(m.data[1:]) for m in tp[1:])[:size]

    # The same frames through python-can-j1939's transport layer.
    got = []
    ecu = j1939.ElectronicControlUnit(send_message=lambda *a, **k: None)
    ecu.subscribe(lambda prio, pgn, src, ts, d: got.append((pgn, src, bytes(d))))
    for m in tp:
        ecu.notify(m.arbitration_id, bytearray(m.data), m.timestamp)
    assert got == [(PGN_DM1, sa, data)]

    assert data[0] & 0x0C == 0x04, f"amber lamp off: {data.hex()}"
    dtcs = {
        (d[0] | d[1] << 8 | (d[2] >> 5) << 16, d[2] & 0x1F)
        for d in (data[i : i + 4] for i in range(2, len(data), 4))
    }
    assert HOT_DTCS <= dtcs, dtcs
