"""The genset node over J1939: address claim, requests and NACK, the engine and
generator PGs at their periods with values that agree across PGs, DM1 growing
from one DTC in a frame to two by BAM, DM2 on request by transport, and SOFT.

Frames are checked directly and decoded with cantools from dbc/genset.dbc.
Transport is reassembled by python-can-j1939, an independent stack: as the
listener of a BAM, and as the receiver granting the windows of an RTS/CTS
session. Each test finds the node's address by requesting Address Claimed, so
none depends on a fresh boot or on an earlier test. Other claimants (a bench
PLC) are tolerated and recorded.

Waits end on frames rather than on host seconds where they can: on Renode the
host clock measures how fast the simulation runs, not the node's timing. The
node's EEC1, every 100 ms, is its clock here.
"""

import contextlib
import itertools
import math
import time

import can
import j1939
import pytest

from zelos_testing import dbc
from zelos_testing import frames as bench
from zelos_testing.frames import (
    Periodic,
    assert_period,
    bound,
    collect,
    until,
)

GENSET = dbc.load("genset")
VCU_COMMAND = dbc.load("bench").get_message_by_name("VCU_Command")


def pgn_of(frame_id):
    return (frame_id >> 8) & 0x3FFFF


# Every message by PGN: the node's address may have moved from the DBC's.
BY_PGN = {pgn_of(m.frame_id): m for m in GENSET.messages}
PERIODIC = [m for m in GENSET.messages if m.cycle_time]
EEC1, ET1, DM1, DM2 = (GENSET.get_message_by_name(n) for n in ("EEC1", "ET1", "DM1", "DM2"))
PGN_EEC1, PGN_ET1, PGN_DM1, PGN_DM2 = (pgn_of(m.frame_id) for m in (EEC1, ET1, DM1, DM2))
PGN_GAAC, PGN_GTACP, PGN_GTACR, PGN_HOURS, PGN_LFC, PGN_LFE, PGN_GTACE, PGN_SOFT = (
    pgn_of(GENSET.get_message_by_name(n).frame_id) for n in ("GAAC", "GTACP", "GTACR", "HOURS", "LFC", "LFE", "GTACE", "SOFT")
)
# Vehicle Identification: a PGN this node has no answer for.
PGN_VI = 65260
PGN = j1939.ParameterGroupNumber.PGN
PGN_ACK = 59392
TP_BAM = 32
TP_RTS = 16
NACK = 1
NULL = j1939.ParameterGroupNumber.Address.NULL
GLOBAL = j1939.ParameterGroupNumber.Address.GLOBAL
# J1939's service tool address: the host plays one.
TESTER = 0xF9

# From nodes/genset: the DTCs (SPN, FMI) of a hot engine, in the order they come.
DTC_COOLANT = (110, 0)
DTC_OIL = (100, 1)
# The generator: 4 poles, 277 V line-neutral at 1800 rpm, power factor 0.8 lagging.
RPM_PER_HZ = 30
V_LN_PER_RPM = 277 / 1800
POWER_FACTOR = 0.8
LAGGING = 1
# The VCU's drive cycle. See nodes/vcu.
VCU_CYCLE_S = 15.0
DRIVE, CHARGE = 1, 2

pytestmark = pytest.mark.usefixtures("bus_health")


def parse(msg):
    """(pgn, da, sa) of a J1939 frame."""
    mid = j1939.MessageId(can_id=msg.arbitration_id)
    pgn = j1939.ParameterGroupNumber()
    pgn.from_message_id(mid)
    da = GLOBAL
    if pgn.is_pdu1_format:
        da, pgn.pdu_specific = pgn.pdu_specific, 0
    return pgn.value, da, mid.source_address


def theirs(msg):
    """A frame from a J1939 node other than us."""
    return msg.is_extended_id and parse(msg)[2] != TESTER


def send(bus, prio, pgn, data, sa=TESTER, da=GLOBAL):
    pg = j1939.ParameterGroupNumber(pgn >> 16, pgn >> 8, pgn)
    if pg.is_pdu1_format:
        pg.pdu_specific = da
    mid = j1939.MessageId(priority=prio, parameter_group_number=pg.value, source_address=sa)
    bus.send(can.Message(arbitration_id=mid.can_id, data=data, is_extended_id=True))


def request(bus, pgn, da=GLOBAL):
    send(bus, 6, PGN.REQUEST, pgn.to_bytes(3, "little"), da=da)


def name_of(msg):
    """The NAME an Address Claimed carries."""
    return j1939.Name(bytes=msg.data).value


def claims(frames):
    """(timestamp, sa, NAME) of every Address Claimed."""
    return [(m.timestamp, parse(m)[2], name_of(m)) for m in frames if parse(m)[0] == PGN.ADDRESSCLAIM]


def claim(bus, sa, name):
    send(bus, 6, PGN.ADDRESSCLAIM, bytes(j1939.Name(value=name).bytes), sa=sa)


def is_from(pgn, sa):
    return lambda m: parse(m)[0] == pgn and parse(m)[2] == sa


def gather(bus, done, node_s=bench.WAIT_S):
    """Frames from J1939 nodes other than us, oldest first, until done(those) holds."""
    got = []

    def step(frames):
        if not theirs(frames[-1]):
            return False
        got.append(frames[-1])
        return done(got)

    until(bus, step, node_s)
    return got


def first(bus, want, node_s=bench.WAIT_S):
    """Frames up to and including the first for which want(msg) holds."""
    return gather(bus, lambda got: want(got[-1]), node_s)


def window(bus, sa, eec1_n):
    """Frames for eec1_n EEC1 periods of the node's time: a tenth of a second each."""
    eec1 = is_from(PGN_EEC1, sa)
    return gather(bus, lambda got: sum(map(eec1, got)) >= eec1_n, eec1_n / 10 + bench.WAIT_S)


def decode(msg):
    """Decoded by the DBC; a DM1 of one DTC is shorter than its two-slot definition."""
    return BY_PGN[parse(msg)[0]].decode(bytes(msg.data), decode_choices=False, allow_truncated=True)


def series(frames, pgn, sa):
    """[(timestamp, decoded)] of one PG from sa, oldest first."""
    return [(m.timestamp, decode(m)) for m in frames if is_from(pgn, sa)(m)]


def bracket(series, t, key):
    """(low, high) of key just before and just after t, or None at the window's edge.

    PGs leave the node a few ticks apart, from one model step or the next:
    a value from another PG at t lies between its neighbours' values.
    """
    before = [v[key] for s, v in series if s <= t]
    after = [v[key] for s, v in series if s >= t]
    if not before or not after:
        return None
    return min(before[-1], after[0]), max(before[-1], after[0])


def between(value, lohi, tol):
    return lohi[0] - tol <= value <= lohi[1] + tol


def dtcs_of(data):
    """{(spn, fmi, oc)} of a DM1/DM2 payload, by python-can-j1939; none when the DTC bytes are zero."""
    found = set()
    for i in range(2, len(data) - 3, 4):
        raw = int.from_bytes(data[i : i + 4], "little")
        if raw & 0xFFFFFF:
            dtc = j1939.DTC(dtc=raw)
            found.add((dtc.spn, dtc.fmi, dtc.oc))
    return found


def lamps(data):
    """(amber warning, red stop) lamp status of a DM1/DM2 payload, decoded by the DBC."""
    d = DM1.decode(bytes(data[:8]), decode_choices=False, allow_truncated=True)
    return d["AmberWarningLamp"], d["RedStopLamp"]


def genset_claim(got):
    """(sa, NAME) of the claimant that then sends EEC1 twice, by when any other has answered too."""
    for t, sa, name in claims(got):
        if sa != NULL and len([m for m in got if m.timestamp > t and is_from(PGN_EEC1, sa)(m)]) >= 2:
            return sa, name
    return None


@pytest.fixture
def node(bus, record_property):
    """(sa, NAME) of the genset, as it answers a request.

    A bench may carry other J1939 nodes (a PLC): their claims are recorded, not
    failed. The genset is the claimant that sends EEC1.
    """
    request(bus, PGN.ADDRESSCLAIM)
    got = gather(bus, lambda got: genset_claim(got) is not None or any(sa == NULL for _, sa, _ in claims(got)))
    found = genset_claim(got)
    assert found is not None, f"the genset could not claim an address: {[(hex(sa), hex(name)) for _, sa, name in claims(got)]}"
    others = sorted({(sa, name) for _, sa, name in claims(got)} - {found})
    record_property("other_claimants", [f"0x{sa:02x} NAME 0x{name:016x}" for sa, name in others])
    return found


@contextlib.contextmanager
def vcu(bus, sa):
    """Play the VCU for the block, unless a real one is on the bus.

    Yields set_mode(mode): what the played VCU commands, or nothing when a real
    one drives its own cycle. A second of the node's time shows a real VCU.
    """
    eec1 = is_from(PGN_EEC1, sa)
    seen = until(bus, lambda frames: frames[-1].arbitration_id == VCU_COMMAND.frame_id or sum(theirs(m) and eec1(m) for m in frames) >= 10)
    if seen[-1].arbitration_id == VCU_COMMAND.frame_id:
        yield lambda mode: None
        return

    state = {"mode": DRIVE, "heartbeat": 0}

    def frame():
        state["heartbeat"] = (state["heartbeat"] + 1) & 0xFF
        data = VCU_COMMAND.encode({"RequestedMode": state["mode"], "TorqueRequest": 150.0, "Heartbeat": state["heartbeat"]})
        return can.Message(arbitration_id=VCU_COMMAND.frame_id, data=data, is_extended_id=False)

    with Periodic(bus, frame, VCU_COMMAND.cycle_time / 1000):
        yield lambda mode: state.update(mode=mode)


def test_claims_address_on_request(node):
    sa, name = node
    # Self-configurable range, so the NAME must say arbitrary-address capable.
    assert 128 <= sa <= 247 and j1939.Name(value=name).arbitrary_address_capable, f"sa 0x{sa:02x} NAME 0x{name:016x}"


def test_defends_address_against_higher_name(bus, node):
    sa, name = node
    claim(bus, sa, name + 1)
    first(bus, lambda m: parse(m)[0] == PGN.ADDRESSCLAIM and (parse(m)[2], name_of(m)) == (sa, name))
    # Still sending from its address after defending it.
    first(bus, is_from(PGN_EEC1, sa))


def test_yields_address_to_lower_name(bus, node):
    sa, name = node
    claim(bus, sa, name - 1)
    t, moved, _ = claims(first(bus, lambda m: parse(m)[0] == PGN.ADDRESSCLAIM and name_of(m) == name))[-1]
    assert moved != sa, "did not yield"
    if moved == NULL:
        # A silent node leaves no frames to count: a host window, at the time scale.
        quiet = [m for m in collect(bus, bound(1.5)) if theirs(m) and parse(m)[0] != PGN.ADDRESSCLAIM]
        assert not quiet, "sent after Cannot Claim Address"
        return

    later = first(bus, lambda m: parse(m)[2] == moved and parse(m)[0] != PGN.ADDRESSCLAIM)
    assert all(parse(m)[2] != sa for m in later if parse(m)[0] != PGN.ADDRESSCLAIM), "still sending from the lost address"
    # J1939-81: 250 ms from claim to first use. The node's clock runs slightly fast.
    if not bench.SIMULATED:
        assert later[-1].timestamp - t >= 0.245, f"used after {(later[-1].timestamp - t) * 1000:.1f} ms"


# The PGs slow enough that a reply stands apart from the periodic send. DM1 is
# asked for by test_dm2_on_request, once it is a frame again.
@pytest.mark.parametrize("msg", [m for m in PERIODIC if m.cycle_time >= 500 and m.name != "DM1"], ids=lambda m: m.name)
def test_answers_request(bus, node, msg):
    sa, _ = node
    mine = is_from(pgn_of(msg.frame_id), sa)

    # Just after a periodic one, the next is half a second or more away: a quick one is the reply.
    first(bus, mine)
    request(bus, pgn_of(msg.frame_id), da=sa)
    t = time.monotonic()
    frames = first(bus, mine)
    if bench.SIMULATED:
        # In the node's time: before its next EEC1 but one, 100 ms apart.
        assert len([m for m in frames if is_from(PGN_EEC1, sa)(m)]) <= 2
    else:
        assert time.monotonic() - t < 0.2
    assert decode(frames[-1])


def test_nacks_a_request_it_cannot_answer(bus, node):
    """J1939-21: a request to the node for a PGN it lacks gets a NACK; a global one, silence."""
    sa, _ = node
    eec1 = is_from(PGN_EEC1, sa)

    request(bus, PGN_VI, da=sa)
    ack = first(bus, is_from(PGN_ACK, sa))[-1]
    assert ack.data[0] == NACK and ack.data[4] == TESTER and int.from_bytes(ack.data[5:8], "little") == PGN_VI, bytes(ack.data).hex()

    request(bus, PGN_VI)
    frames = gather(bus, lambda got: sum(map(eec1, got)) >= 3)
    assert not [m for m in frames if is_from(PGN_ACK, sa)(m)], "answered a global request for a PGN it lacks"


def test_periods(bus, node):
    sa, _ = node
    et1 = is_from(PGN_ET1, sa)
    # From one ET1, the slowest message, to the third after it.
    frames = first(bus, et1)[-1:]
    frames += gather(bus, lambda got: sum(map(et1, got)) >= 3)

    def dm1(m):
        # Hot, DM1 goes by BAM: count its announcement.
        return is_from(PGN_DM1, sa)(m) or (is_from(PGN.TP_CM, sa)(m) and m.data[0] == TP_BAM and int.from_bytes(m.data[5:8], "little") == PGN_DM1)

    for msg in PERIODIC:
        mine = dm1 if msg.name == "DM1" else is_from(pgn_of(msg.frame_id), sa)
        stamps = [m.timestamp for m in frames if mine(m)]
        # On any clock the periods keep their ratio to ET1's, +-1 for jitter.
        want = 3 * ET1.cycle_time / msg.cycle_time
        assert abs(len(stamps) - want) <= 1 + want / 50, f"{len(stamps)} {msg.name} in three ET1 periods, {want:.0f} expected"
        # 2%: an untrimmed node clock; the host stamps on receipt.
        assert_period([b - a for a, b in itertools.pairwise(stamps)], msg.cycle_time / 1000, 0.02)


def test_values_agree_across_pgs(bus, node):
    """One model behind every PG: frequency and voltage from speed, P = 3 V I pf, S = P / pf,
    Q = sqrt(S^2 - P^2), every phase the average, fuel burning only while turning."""
    sa, _ = node
    frames = window(bus, sa, 30)
    eec1 = series(frames, PGN_EEC1, sa)
    gaac = series(frames, PGN_GAAC, sa)
    gtacr = series(frames, PGN_GTACR, sa)
    assert len(eec1) >= 30 and len(gaac) >= 25 and len(gtacr) >= 25

    for t, g in gaac:
        rpm = bracket(eec1, t, "EngineSpeed")
        if rpm is None:
            continue
        v_ln, v_ll = g["GeneratorAverageLineNeutralACRMSVoltage"], g["GeneratorAverageLineLineACRMSVoltage"]
        # 1/128 Hz and 1 V of resolution, truncated, on top of the speed's 1/8 rpm.
        assert between(g["GeneratorAverageACFrequency"], (rpm[0] / RPM_PER_HZ, rpm[1] / RPM_PER_HZ), 0.02), (g, rpm)
        assert between(v_ln, (rpm[0] * V_LN_PER_RPM, rpm[1] * V_LN_PER_RPM), 1.1), (g, rpm)
        assert abs(v_ll - v_ln * math.sqrt(3)) <= 2, g

    for t, p in series(frames, PGN_GTACP, sa):
        real, apparent = p["GeneratorTotalRealPower"], p["GeneratorTotalApparentPower"]
        r = bracket(gtacr, t, "GeneratorTotalReactivePower")
        pf = bracket(gtacr, t, "GeneratorOverallPowerFactor")
        v = bracket(gaac, t, "GeneratorAverageLineNeutralACRMSVoltage")
        i = bracket(gaac, t, "GeneratorAverageACRMSCurrent")
        if None in (r, pf, v, i):
            continue
        assert abs(pf[0] - POWER_FACTOR) < 1e-3 and pf[0] == pf[1], pf
        assert abs(apparent - real / POWER_FACTOR) <= 2, p
        assert between(math.sqrt(apparent**2 - real**2), r, 3), (p, r)
        # V and I are whole units, truncated: a unit of each, times 3 pf.
        lo, hi = (3 * v[k] * i[k] * POWER_FACTOR for k in (0, 1))
        assert between(real, (min(lo, hi), max(lo, hi)), 3 * POWER_FACTOR * (v[1] + i[1] + 1) + 1), (p, v, i)
    assert all(r["GeneratorOverallPowerFactorLagging"] == LAGGING for _, r in gtacr)

    avg = {"GeneratorAverageLineLineACRMSVoltage": "LineLineACRMSVoltage", "GeneratorAverageLineNeutralACRMSVoltage": "LineNeutralACRMSVoltage", "GeneratorAverageACFrequency": "ACFrequency", "GeneratorAverageACRMSCurrent": "ACRMSCurrent"}
    for name in ("GPAAC", "GPBAC", "GPCAC"):
        for t, ph in series(frames, pgn_of(GENSET.get_message_by_name(name).frame_id), sa):
            for key, suffix in avg.items():
                (value,) = (v for k, v in ph.items() if k.endswith(suffix))
                assert between(value, bracket(gaac, t, key) or (value, value), 0.02 if suffix == "ACFrequency" else 1), (name, ph)

    for t, f in series(frames, PGN_LFE, sa):
        rpm = bracket(eec1, t, "EngineSpeed")
        if rpm is not None and rpm[0] > 0:
            assert f["EngineFuelRate"] > 0, f
        if rpm is not None and rpm[1] == 0:
            assert f["EngineFuelRate"] == 0, f


def test_counters_integrate(bus, node):
    """Hours, revolutions, fuel and energy advance with the speed, fuel rate and
    power seen, within a bit of their resolution, and never go back.

    The integrals run in the node's time: each EEC1, LFE and GTACP stands for
    its 100 ms period.
    """
    sa, _ = node
    frames = window(bus, sa, 200)
    hours = series(frames, PGN_HOURS, sa)
    lfc = series(frames, PGN_LFC, sa)
    gtace = series(frames, PGN_GTACE, sa)
    assert min(len(hours), len(lfc), len(gtace)) >= 19

    def delta(s, key):
        values = [v[key] for _, v in s]
        assert values == sorted(values), key
        return values[-1] - values[0], s[0][0], s[-1][0]

    def integral(pgn, key, t0, t1):
        """Sum of key over the PG's frames between t0 and t1, each worth 0.1 s."""
        return sum(v[key] * 0.1 for t, v in series(frames, pgn, sa) if t0 <= t <= t1)

    # Resolutions: 0.05 h, 1000 r, 0.5 L, 1 kWh.
    d, t0, t1 = delta(hours, "EngineTotalHoursOfOperation")
    run_s = sum(0.1 for t, v in series(frames, PGN_EEC1, sa) if t0 <= t <= t1 and v["EngineSpeed"] > 0)
    assert abs(d - run_s / 3600) <= 0.05, (d, run_s)
    d, t0, t1 = delta(hours, "EngineTotalRevolutions")
    assert abs(d - integral(PGN_EEC1, "EngineSpeed", t0, t1) / 60) <= 1000, d
    for key in ("EngineTotalFuelUsed", "EngineTripFuel"):
        d, t0, t1 = delta(lfc, key)
        assert abs(d - integral(PGN_LFE, "EngineFuelRate", t0, t1) / 3600) <= 0.5, (key, d)
    d, t0, t1 = delta(gtace, "GeneratorTotalkWhExport")
    assert abs(d - integral(PGN_GTACP, "GeneratorTotalRealPower", t0, t1) / 3.6e6) <= 1, d
    assert all(v["GeneratorTotalkWhImport"] == 0 for _, v in gtace)


def listener():
    """A python-can-j1939 ECU that only listens, and what it reassembled: (pgn, sa, data)."""
    got = []
    ecu = j1939.ElectronicControlUnit(send_message=lambda *a, **k: None)
    ecu.subscribe(lambda prio, pgn, src, ts, d: got.append((pgn, src, bytes(d))))
    return ecu, got


def bam_of(pgn, sa):
    return lambda m: is_from(PGN.TP_CM, sa)(m) and m.data[0] == TP_BAM and int.from_bytes(m.data[5:8], "little") == pgn


def reassemble_bam(bus, sa, cm, node_s=bench.WAIT_S):
    """The payload a BAM announced by cm carries, by python-can-j1939; its frames checked on the way."""
    packets = cm.data[3]
    frames = [cm] + first(bus, lambda m: is_from(PGN.DATATRANSFER, sa)(m) and m.data[0] == packets, node_s)
    tp = [m for m in frames if parse(m)[2] == sa and parse(m)[0] in (PGN.TP_CM, PGN.DATATRANSFER)]
    assert [m.data[0] for m in tp[1:]] == list(range(1, packets + 1)), [bytes(m.data).hex() for m in tp]

    # J1939-21: 50 to 200 ms between BAM packets. Under Renode the host stamps
    # carry jitter of the order of the spacing itself, so there only the high
    # end holds, at the host's time scale; the low end is checked on hardware.
    gaps = [b.timestamp - a.timestamp for a, b in itertools.pairwise(tp)]
    if bench.SIMULATED:
        lower, upper = 0.0, 0.2 * bench.TIME_SCALE + bench.HOST_JITTER_S
    else:
        lower, upper = 0.05, 0.2
    assert all(lower <= g <= upper for g in gaps), [round(g * 1000, 1) for g in gaps]

    ecu, got = listener()
    for m in tp:
        ecu.notify(m.arbitration_id, bytearray(m.data), m.timestamp)
    ecu.stop()
    assert [(pgn, src) for pgn, src, _ in got] == [(int.from_bytes(cm.data[5:8], "little"), sa)], got
    return got[0][2]


def test_dm1_one_dtc_in_a_frame_then_two_by_bam(bus, node):
    """Drive the engine hot: DM1 carries the coolant DTC in a frame with the
    amber lamp, then the oil pressure DTC joins it, red lamp on, and DM1 goes
    by BAM. Cooled, DM1 is empty; hot again, the coolant DTC counts one more.

    Plays the VCU, unless a real one is on the bus: then its drive cycle heats
    and cools the engine. Bounds are in the node's time, counted in its EEC1.
    """
    sa, _ = node
    eec1 = is_from(PGN_EEC1, sa)
    cycle = VCU_CYCLE_S * 1000 / EEC1.cycle_time

    def within(cycles, want):
        """Frames to the first that want() accepts, within so many VCU cycles."""
        frames = gather(bus, lambda got: want(got[-1]) or sum(map(eec1, got)) > cycles * cycle, cycles * VCU_CYCLE_S + bench.WAIT_S)
        assert want(frames[-1]), f"not seen within {cycles} drive cycles"
        return frames[-1]

    def one_dtc(m):
        return is_from(PGN_DM1, sa)(m) and {d[:2] for d in dtcs_of(m.data)} == {DTC_COOLANT}

    def no_dtc(m):
        return is_from(PGN_DM1, sa)(m) and not dtcs_of(m.data)

    with vcu(bus, sa) as set_mode:
        set_mode(DRIVE)
        single = within(2, one_dtc)
        ((*_, oc),) = dtcs_of(single.data)
        assert lamps(single.data) == (1, 0), bytes(single.data).hex()
        assert len(single.data) == 8 and bytes(single.data[6:8]) == b"\xff\xff"

        data = reassemble_bam(bus, sa, within(1, bam_of(PGN_DM1, sa)))
        assert len(data) == 10 and lamps(data) == (1, 1), data.hex()
        assert {d[:2] for d in dtcs_of(data)} == {DTC_COOLANT, DTC_OIL}
        assert (*DTC_COOLANT, oc) in dtcs_of(data), (dtcs_of(data), oc)

        set_mode(CHARGE)
        cleared = within(1, no_dtc)
        assert lamps(cleared.data) == (0, 0) and bytes(cleared.data[6:8]) == b"\xff\xff"

        set_mode(DRIVE)
        again = within(2, one_dtc)
        assert (*DTC_COOLANT, oc + 1) in dtcs_of(again.data), (dtcs_of(again.data), oc)


def test_dm2_on_request(bus, node):
    """Idle, both faults are history: DM2 lists them, by BAM to a global request
    and by RTS/CTS to ours, where python-can-j1939 grants the windows. DM1,
    asked for, is a frame of no DTC.
    """
    sa, _ = node

    def no_dtc(m):
        return is_from(PGN_DM1, sa)(m) and not dtcs_of(m.data)

    first(bus, no_dtc, 2 * VCU_CYCLE_S + bench.WAIT_S)
    request(bus, PGN_DM1, da=sa)
    frames = first(bus, is_from(PGN_DM1, sa))
    assert len([m for m in frames if is_from(PGN_EEC1, sa)(m)]) <= 2 and no_dtc(frames[-1])

    request(bus, PGN_DM2)
    by_bam = reassemble_bam(bus, sa, first(bus, bam_of(PGN_DM2, sa))[-1])
    assert len(by_bam) == 10 and lamps(by_bam) == (1, 1), by_bam.hex()
    history = dtcs_of(by_bam)
    assert {d[:2] for d in history} == {DTC_COOLANT, DTC_OIL} and all(oc >= 1 for _, _, oc in history), history

    # python-can-j1939 as the receiver: it answers the RTS with CTS, one packet a window, and ends with EOMA.
    got = []
    ecu = j1939.ElectronicControlUnit(send_message=lambda can_id, ext, data, fd_format=False: bus.send(can.Message(arbitration_id=can_id, data=bytes(data), is_extended_id=True)))

    def on_pg(prio, pgn, src, ts, d):
        # The ECU hands every PG to its subscribers, single frames included.
        if pgn == PGN_DM2:
            got.append((pgn, src, bytes(d)))

    ecu.subscribe(on_pg, device_address=TESTER)
    try:
        request(bus, PGN_DM2, da=sa)
        frames = gather(bus, lambda frames: ecu.notify(frames[-1].arbitration_id, bytearray(frames[-1].data), frames[-1].timestamp) or bool(got))
    finally:
        ecu.stop()
    tp = [m for m in frames if parse(m)[2] == sa and parse(m)[0] in (PGN.TP_CM, PGN.DATATRANSFER)]
    assert tp and all(parse(m)[1] == TESTER for m in tp), [bytes(m.data).hex() for m in tp]
    assert tp[0].data[0] == TP_RTS and len(tp) == 1 + tp[0].data[3]
    assert got == [(PGN_DM2, sa, by_bam)], got


def software_id(data):
    """The fields of a J1939-71 Software Identification: count byte, then '*'-terminated ASCII."""
    fields = bytes(data[1:]).split(b"*")
    assert fields[-1] == b"" and len(fields) - 1 == data[0], bytes(data)
    return [f.decode("ascii") for f in fields[:-1]]


def test_software_id_by_rts_cts_to_the_requester(bus, node):
    """A request addressed to the genset gets SOFT by RTS/CTS, to us alone, in the windows we grant."""
    sa, _ = node
    request(bus, PGN_SOFT, da=sa)
    # Its RTS for SOFT: an earlier session of ours may still be aborting.
    rts = first(bus, lambda m: is_from(PGN.TP_CM, sa)(m) and parse(m)[1] == TESTER and m.data[0] == TP_RTS)[-1]
    assert int.from_bytes(rts.data[5:8], "little") == PGN_SOFT, bytes(rts.data).hex()
    size, packets = int.from_bytes(rts.data[1:3], "little"), rts.data[3]

    def cm(*head):
        send(bus, 7, PGN.TP_CM, bytes([*head, 0xFF, 0xFF][:5]) + rts.data[5:8], da=sa)

    # Two windows: one packet, then the rest.
    data = {}
    for n, nxt in ((1, 1), (packets - 1, 2)):
        cm(17, n, nxt)
        dts = gather(bus, lambda got: sum(is_from(PGN.DATATRANSFER, sa)(m) for m in got) == n)
        data |= {m.data[0]: bytes(m.data[1:]) for m in dts if is_from(PGN.DATATRANSFER, sa)(m)}
    cm(19, size & 0xFF, size >> 8, packets)
    assert sorted(data) == list(range(1, packets + 1))
    fields = software_id(b"".join(data[k] for k in sorted(data))[:size])
    assert fields[0] == "ZELOS-GENSET", fields


def test_software_id_by_bam_to_everyone(bus, node):
    """A global request gets SOFT by BAM."""
    sa, _ = node
    request(bus, PGN_SOFT)
    cm = first(bus, lambda m: is_from(PGN.TP_CM, sa)(m) and m.data[0] == TP_BAM)[-1]
    packets = cm.data[3]
    dts = gather(bus, lambda got: sum(is_from(PGN.DATATRANSFER, sa)(m) for m in got) == packets)
    payload = b"".join(bytes(m.data[1:]) for m in dts if is_from(PGN.DATATRANSFER, sa)(m))
    assert software_id(payload[: int.from_bytes(cm.data[1:3], "little")])[0] == "ZELOS-GENSET"
