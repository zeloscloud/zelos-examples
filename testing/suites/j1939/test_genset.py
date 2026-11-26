"""The genset node over J1939: address claim, requests and NACK, the engine and
generator PGs at their periods with values that agree across PGs, the status
fields following the mode, a controller's TSC1 overriding the governor, DM1
growing from one DTC in a frame to two by BAM, DM2 on request by transport,
SOFT and CI, and the rest of J1939-73 a service tool asks for: the other DTC
lists with counts that agree, readiness, calibration, a freeze frame, clearing
DTCs, a commanded test and memory access behind a seed and key.

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


# The node's messages by PGN: its address may have moved from the DBC's. TSC1 is what it receives.
SENT = [m for m in GENSET.messages if "GENSET" in m.senders]
BY_PGN = {pgn_of(m.frame_id): m for m in SENT}
PERIODIC = [m for m in SENT if m.cycle_time]
EEC1, ET1, DM1, DM2, TSC1 = (GENSET.get_message_by_name(n) for n in ("EEC1", "ET1", "DM1", "DM2", "TSC1"))
PGN_EEC1, PGN_ET1, PGN_DM1, PGN_DM2 = (pgn_of(m.frame_id) for m in (EEC1, ET1, DM1, DM2))
PGN_GAAC, PGN_GTACP, PGN_GTACR, PGN_HOURS, PGN_LFC, PGN_LFE, PGN_GTACE, PGN_SOFT, PGN_EEC2, PGN_EEC3, PGN_CCVS, PGN_CI = (
    pgn_of(GENSET.get_message_by_name(n).frame_id) for n in ("GAAC", "GTACP", "GTACR", "HOURS", "LFC", "LFE", "GTACE", "SOFT", "EEC2", "EEC3", "CCVS1", "CI")
)
PGN_TSC1 = 0
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
STANDBY, DRIVE, CHARGE = 0, 1, 2
# From nodes/genset: idle and rated speed, and what EEC1 and CCVS1 say in each mode.
IDLE_RPM, RATED_RPM = 900, 1800
TORQUE_MODE_LOW_IDLE, TORQUE_MODE_PEDAL, TORQUE_MODE_LIMITING, TORQUE_MODE_REMOTE = 0, 1, 8, 11
STARTER_INHIBITED_RUN = 4
PTO_STANDBY, PTO_SET = 3, 5
TSC1_OFF, TSC1_SPEED, TSC1_LIMIT = 0, 1, 3

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


def test_status_fields_follow_the_mode(bus, node):
    """In standby the engine idles under its low idle governor, pedal at idle,
    parking brake set, PTO in standby; driving, the pedal governs, brake off,
    PTO set; EEC3's desired speed is the governor's target either way. The
    engine itself is the controlling device, and the starter is done."""
    sa, _ = node

    def settled(mode, rpm):
        def check(got):
            eec1 = series(got, PGN_EEC1, sa)
            # Settled for three EEC1, and the slowest PG looked at has come by.
            return len(eec1) >= 3 and all(abs(v["EngineSpeed"] - rpm) < 1 for _, v in eec1[-3:]) and bool(series(got, PGN_EEC3, sa))

        frames = gather(bus, check, 2 * VCU_CYCLE_S + bench.WAIT_S)
        latest = {pgn: series(frames, pgn, sa)[-1][1] for pgn in (PGN_EEC1, PGN_EEC2, PGN_EEC3, PGN_CCVS)}
        assert latest[PGN_EEC1]["EngineTorqueMode"] == mode and latest[PGN_EEC1]["EngineStarterMode"] == STARTER_INHIBITED_RUN, latest[PGN_EEC1]
        assert latest[PGN_EEC1]["SourceAddressOfControllingDeviceForEngineControl"] == sa
        assert abs(latest[PGN_EEC3]["EnginesDesiredOperatingSpeed"] - rpm) < 1, latest[PGN_EEC3]
        return latest

    with vcu(bus, sa) as set_mode:
        set_mode(STANDBY)
        idle = settled(TORQUE_MODE_LOW_IDLE, IDLE_RPM)
        assert idle[PGN_EEC2]["AcceleratorPedal1LowIdleSwitch"] == 1
        assert idle[PGN_CCVS]["ParkingBrakeSwitch"] == 1 and idle[PGN_CCVS]["PTOGovernorState"] == PTO_STANDBY, idle[PGN_CCVS]

        set_mode(DRIVE)
        drive = settled(TORQUE_MODE_PEDAL, RATED_RPM)
        assert drive[PGN_EEC2]["AcceleratorPedal1LowIdleSwitch"] == 0
        assert drive[PGN_CCVS]["ParkingBrakeSwitch"] == 0 and drive[PGN_CCVS]["PTOGovernorState"] == PTO_SET, drive[PGN_CCVS]
        assert drive[PGN_CCVS]["BrakeSwitch"] == 0 and drive[PGN_CCVS]["CruiseControlActive"] == 0


def tsc1(bus, sa, mode, rpm, torque=None):
    """A TSC1 to the node every 10 ms, as a controller sends it, for the block."""
    data = TSC1.encode(
        {
            "EngineOverrideControlMode": mode,
            "EngineRequestedSpeedControlConditions": 0,
            "OverrideControlModePriority": 0,
            "EngineRequestedSpeedSpeedLimit": rpm,
            "EngineRequestedTorqueTorqueLimit": 0 if torque is None else torque,
            "TSC1TransmissionRate": 7,
            "TSC1ControlPurpose": 31,
            "EngineRequestedTorqueHighResolution": 0.875,
            "MessageCounter": 15,
            "MessageChecksum": 15,
        }
    )
    # Bits the DBC leaves out are not available: ones; so is a torque not given.
    data = bytes(b | n for b, n in zip(data, b"\xc0\x00\x00" + (b"\xff" if torque is None else b"\x00") + b"\x00\xf0\xff\x00"))
    pg = j1939.ParameterGroupNumber(0, 0, sa)
    mid = j1939.MessageId(priority=3, parameter_group_number=pg.value, source_address=TESTER)
    return Periodic(bus, lambda: can.Message(arbitration_id=mid.can_id, data=data, is_extended_id=True), 0.01)


def test_tsc1_overrides_the_governor(bus, node):
    """Speed control from a tester takes the engine from idle to the requested
    speed, EEC1 naming the tester as the controlling device under remote
    control; a speed limit below idle holds it at idle; when TSC1 stops, the
    governor and the node's own address are back within its timeout."""
    sa, _ = node
    target = 1500

    def at(rpm, mode, controller):
        def check(got):
            eec1 = series(got, PGN_EEC1, sa)
            return len(eec1) >= 3 and all(
                abs(v["EngineSpeed"] - rpm) < 1 and v["EngineTorqueMode"] == mode and v["SourceAddressOfControllingDeviceForEngineControl"] == controller for _, v in eec1[-3:]
            )

        frames = gather(bus, lambda got: check(got) and bool(series(got, PGN_EEC3, sa)), 2 * VCU_CYCLE_S + bench.WAIT_S)
        return series(frames, PGN_EEC3, sa)[-1][1]

    with vcu(bus, sa) as set_mode:
        set_mode(STANDBY)
        at(IDLE_RPM, TORQUE_MODE_LOW_IDLE, sa)
        with tsc1(bus, sa, TSC1_SPEED, target):
            eec3 = at(target, TORQUE_MODE_REMOTE, TESTER)
            assert abs(eec3["EnginesDesiredOperatingSpeed"] - target) < 1, eec3
        # Gone: back under the low idle governor, its own address again.
        at(IDLE_RPM, TORQUE_MODE_LOW_IDLE, sa)
        with tsc1(bus, sa, TSC1_LIMIT, IDLE_RPM - 300):
            at(IDLE_RPM, TORQUE_MODE_LIMITING, TESTER)


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


def test_component_id_by_bam_to_everyone(bus, node):
    """A global request gets CI by BAM: make, model, serial and unit number, '*'-terminated."""
    sa, _ = node
    request(bus, PGN_CI)
    data = reassemble_bam(bus, sa, first(bus, bam_of(PGN_CI, sa))[-1])
    fields = bytes(data).split(b"*")
    assert fields[-1] == b"" and len(fields) == 5, data
    assert fields[0] == b"Zelos" and fields[1] == b"GENSET-FULL", data


# J1939-73 beyond DM1 and DM2. The fixed 8-byte ones are in the DBC; the rest are parsed here.
DM5, DM7, DM14, DM15, DM21, DM22, DM26, DM29, DM34 = (GENSET.get_message_by_name(n) for n in ("DM5", "DM7", "DM14", "DM15", "DM21", "DM22", "DM26", "DM29", "DM34"))
PGN_DM3, PGN_DM5, PGN_DM11, PGN_DM26 = 65228, 65230, 65235, 64952
PGN_DM7, PGN_DM14, PGN_DM15, PGN_DM16, PGN_DM22, PGN_DM30 = 58112, 55552, 55296, 55040, 49920, 41984
PGN_DM19, PGN_DM20, PGN_DM21, PGN_DM24, PGN_DM25, PGN_DM29, PGN_DM31, PGN_DM33, PGN_DM34, PGN_DM56 = 54016, 49664, 49408, 64950, 64951, 40448, 41728, 41216, 40960, 64711
# The DTC lists, all laid out as DM1.
DTC_LISTS = {"DM1": PGN_DM1, "DM2": PGN_DM2, "DM6": 65231, "DM12": 65236, "DM23": 64949, "DM27": 64898, "DM28": 64896}
ACK = 0
NO_RECORD = bytes.fromhex("0000000000ffffff")
# From nodes/genset: its memory, 64 bytes at pointer 0, byte n starting as 0xA0 + n, and its key for a seed.
MEMORY = bytes(0xA0 + n for n in range(64))
MEM_KEY = 0x1939
MEM_READ, MEM_WRITE, MEM_COMPLETED = 1, 2, 4
MEM_PROCEED, MEM_FAILED = 0, 5
NO_KEY = 0xFFFF
# The freeze frame's SPNs, in DM24's order: engine speed, coolant temperature, oil pressure, load.
FREEZE_SPNS = [190, 110, 100, 92]
HOT_ON_C = 105


def pdu(pgn, data, da, prio=6):
    """A PG from us to da, not yet sent."""
    pg = j1939.ParameterGroupNumber(pgn >> 16, pgn >> 8, pgn)
    if pg.is_pdu1_format:
        pg.pdu_specific = da
    mid = j1939.MessageId(priority=prio, parameter_group_number=pg.value, source_address=TESTER)
    return can.Message(arbitration_id=mid.can_id, data=data, is_extended_id=True)


def answer(bus, sa, pgn, to=None, node_s=bench.WAIT_S):
    """The payload of the next PG pgn from the node: a frame, or by transport, where
    python-can-j1939 grants the windows of an RTS/CTS session to us.

    to: a frame of ours to send first; only what follows it on the bus counts.
    """
    got = []
    ecu = j1939.ElectronicControlUnit(send_message=lambda can_id, ext, data, fd_format=False: bus.send(can.Message(arbitration_id=can_id, data=bytes(data), is_extended_id=True)))

    def on_pg(prio, p, src, ts, d):
        # Reassembled only: a single frame is taken off the bus below. A PDU1 PGN may carry its destination.
        if src == sa and len(d) > 8 and p in (pgn, pgn | TESTER, pgn | GLOBAL):
            got.append(bytes(d))

    ecu.subscribe(on_pg, device_address=TESTER)
    armed = to is None

    def step(frames):
        nonlocal armed
        m = frames[-1]
        if not m.is_extended_id:
            return False
        if not armed:
            armed = m.arbitration_id == to.arbitration_id and bytes(m.data) == bytes(to.data)
        elif theirs(m):
            if is_from(pgn, sa)(m):
                got.append(bytes(m.data))
            else:
                ecu.notify(m.arbitration_id, bytearray(m.data), m.timestamp)
        return bool(got)

    try:
        if to is not None:
            bus.send(to)
        until(bus, step, node_s)
    finally:
        ecu.stop()
    return got[0]


def ask(bus, sa, pgn):
    """What the node answers our request for pgn with."""
    return answer(bus, sa, pgn, pdu(PGN.REQUEST, pgn.to_bytes(3, "little"), sa))


def dtc_lists(bus, sa):
    """{DM: {(spn, fmi)}} of every DTC list, each asked for."""
    return {name: {d[:2] for d in dtcs_of(ask(bus, sa, pgn))} for name, pgn in DTC_LISTS.items()}


def assert_counts(bus, sa, lists):
    """DM5 and DM29 count what the lists hold."""
    dm5 = DM5.decode(ask(bus, sa, PGN_DM5), decode_choices=False)
    assert (dm5["ActiveDtcCount"], dm5["PreviouslyActiveDtcCount"]) == (len(lists["DM1"]), len(lists["DM2"])), (dm5, lists)
    dm29 = DM29.decode(ask(bus, sa, PGN_DM29), decode_choices=False)
    counts = [dm29[k] for k in ("PendingDtcCount", "AllPendingDtcCount", "MilOnDtcCount", "PreviouslyMilOnDtcCount", "PermanentDtcCount")]
    assert counts == [len(lists[k]) for k in ("DM6", "DM27", "DM12", "DM23", "DM28")], (dm29, lists)


def spn_support(data):
    """[(spn, in the freeze frame, in the data stream, in test results, length)] of a DM24, in its order."""
    assert len(data) % 4 == 0, data.hex()
    return [(r[0] | r[1] << 8 | (r[2] >> 5) << 16, not r[2] & 1, not r[2] & 2, not r[2] & 4, r[3]) for r in (data[i : i + 4] for i in range(0, len(data), 4))]


def check_dm19(data):
    assert len(data) == 20 and int.from_bytes(data[:4], "little") == 0x0A11CA1B, data.hex()
    assert data[4:].rstrip(b"\0 ") == b"ZELOS-GENSET", data


def check_dm20(data):
    assert len(data) == 4 + 7 * 3 and int.from_bytes(data[:2], "little") == 120 and int.from_bytes(data[2:4], "little") == 100, data.hex()
    ratios = {int.from_bytes(r[:3], "little") & 0x7FFFF: (int.from_bytes(r[3:5], "little"), int.from_bytes(r[5:7], "little")) for r in (data[i : i + 7] for i in range(4, len(data), 7))}
    # Engine speed has no monitor: numerator and denominator not available.
    assert ratios == {110: (95, 100), 100: (98, 100), 190: (0xFFFF, 0xFFFF)}, ratios


def check_dm24(data):
    spns = spn_support(data)
    assert [spn for spn, freeze, *_ in spns if freeze] == FREEZE_SPNS, spns
    assert {spn for spn, _, _, tested, _ in spns if tested} == {110, 100} and all(stream for _, _, stream, _, _ in spns), spns


def check_dm31(data):
    """Each DTC that is or was active with the lamp it lights: amber for coolant, red for oil pressure."""
    assert len(data) == 12, data.hex()
    lamp = {(*next(iter(dtcs_of(b"\0\0" + r[:4])))[:2],): r[4] for r in (data[:6], data[6:])}
    assert lamp == {DTC_COOLANT: 0x04, DTC_OIL: 0x10} and data[5] == data[11] == 0xFF, data.hex()


def check_dm33(data):
    assert len(data) == 9 and data[0] == 1 and data[5:] == b"\xff" * 4, data.hex()


def check_dm34(data):
    d = DM34.decode(data, decode_choices=False)
    # Idling: outside both control areas; deficiency areas not available.
    assert (d["NoxControlArea"], d["PmControlArea"], d["NoxDeficiencyArea"], d["PmDeficiencyArea"]) == (0, 0, 3, 3), d
    assert data[0] & 3 == data[1] & 3 == 3 and data[2:] == b"\xff" * 6, data.hex()


def check_dm56(data):
    assert data[:8] == b"2026E-MY" and data.endswith(b"*") and data[8:-1].decode("ascii"), data


def check_dm21(data):
    d = DM21.decode(data, decode_choices=False)
    # No MIL, and a genset goes nowhere.
    assert d["DistanceMilOn"] == d["DistanceSinceClear"] == d["TimeMilOn"] == 0 and d["TimeSinceClear"] <= 64255, d


def check_dm26(data):
    # Fuel system and comprehensive component monitors complete; exhaust gas sensor complete, boost pressure not.
    assert data[3:] == bytes.fromhex("0620020002") and int.from_bytes(data[:2], "little") <= 64255, data.hex()


@pytest.mark.parametrize(
    "pgn, check",
    [(PGN_DM19, check_dm19), (PGN_DM20, check_dm20), (PGN_DM21, check_dm21), (PGN_DM24, check_dm24), (PGN_DM26, check_dm26), (PGN_DM31, check_dm31), (PGN_DM33, check_dm33), (PGN_DM34, check_dm34), (PGN_DM56, check_dm56)],
    ids=["DM19", "DM20", "DM21", "DM24", "DM26", "DM31", "DM33", "DM34", "DM56"],
)
def test_answers_diagnostic_request(bus, node, pgn, check):
    """Asked, the genset answers us: a frame, or RTS/CTS for the longer ones. Its engine idles under no VCU or a played one."""
    sa, _ = node
    with vcu(bus, sa) as set_mode:
        set_mode(STANDBY)
        # Idle and cooled, as DM31 and DM34 are checked.
        first(bus, lambda m: is_from(PGN_DM1, sa)(m) and not dtcs_of(m.data), 2 * VCU_CYCLE_S + bench.WAIT_S)
        check(ask(bus, sa, pgn))


def test_pdu1_answer_goes_to_the_requester(bus, node):
    """A global request for a PDU1 PG is answered to whoever asked, not to everyone."""
    sa, _ = node
    request(bus, PGN_DM29)
    reply = first(bus, is_from(PGN_DM29, sa))[-1]
    assert parse(reply)[1] == TESTER and len(reply.data) == 8, hex(reply.arbitration_id)


def test_dtc_lists_counts_and_clearing(bus, node):
    """Idle, the faults are history: every list holds what its DM names and DM5
    and DM29 count them. One is cleared by DM22, the rest by DM3. Hot, both are
    active and pending, DM27 goes by transport and DM25 holds the frame frozen
    when the coolant fault set, laid out as DM24 says. DM11 clears them; they
    are back at once, counted from one. Cooled, the history is as it began.
    """
    sa, _ = node
    eec1 = is_from(PGN_EEC1, sa)
    both = {DTC_COOLANT, DTC_OIL}

    def no_dtc(m):
        return is_from(PGN_DM1, sa)(m) and not dtcs_of(m.data)

    def dm22(control, dtc):
        return pdu(PGN_DM22, bytes([control, 0xFF, 0xFF, 0xFF, 0xFF, dtc[0] & 0xFF, dtc[0] >> 8, dtc[1]]), sa)

    def acks(frames):
        return [bytes(m.data) for m in frames if is_from(PGN_ACK, sa)(m)]

    with vcu(bus, sa) as set_mode:
        set_mode(STANDBY)
        first(bus, no_dtc, 2 * VCU_CYCLE_S + bench.WAIT_S)
        lists = dtc_lists(bus, sa)
        assert lists == {"DM1": set(), "DM2": both, "DM6": set(), "DM12": set(), "DM23": {DTC_COOLANT}, "DM27": set(), "DM28": both}, lists
        assert_counts(bus, sa, lists)
        assert ask(bus, sa, PGN_DM25) == NO_RECORD

        # DM22: (command, DTC) and the control byte and indicator of the answer.
        for control, dtc, reply in [
            (1, DTC_COOLANT, (2, 0xFF)),  # previously active: cleared
            (1, DTC_COOLANT, (3, 3)),  # no longer previously active
            (17, DTC_OIL, (19, 4)),  # not active
            (17, (190, 0), (19, 2)),  # not a DTC of this node
        ]:
            got = answer(bus, sa, PGN_DM22, dm22(control, dtc))
            assert tuple(got[:2]) == reply and got[2:] == bytes(dm22(control, dtc).data[2:]), (control, dtc, got.hex())
        assert {d[:2] for d in dtcs_of(ask(bus, sa, PGN_DM2))} == {DTC_OIL}

        # DM3 to everyone: acted on, not acknowledged.
        request(bus, PGN_DM3)
        assert not acks(gather(bus, lambda got: sum(map(eec1, got)) >= 3))
        lists = dtc_lists(bus, sa)
        assert lists["DM2"] == lists["DM23"] == set() and lists["DM28"] == both, lists
        assert_counts(bus, sa, lists)
        assert ask(bus, sa, PGN_DM31) == NO_RECORD

        set_mode(DRIVE)
        first(bus, bam_of(PGN_DM1, sa), 2 * VCU_CYCLE_S + bench.WAIT_S)
        lists = dtc_lists(bus, sa)
        assert lists == {"DM1": both, "DM2": set(), "DM6": {DTC_COOLANT}, "DM12": {DTC_COOLANT}, "DM23": set(), "DM27": both, "DM28": both}, lists
        assert_counts(bus, sa, lists)

        frozen = ask(bus, sa, PGN_DM25)
        sizes = [size for _, freeze, _, _, size in spn_support(ask(bus, sa, PGN_DM24)) if freeze]
        assert frozen[0] == len(frozen) - 1 == 4 + sum(sizes), frozen.hex()
        assert {d[:2] for d in dtcs_of(b"\0\0" + frozen[1:5])} == {DTC_COOLANT}, frozen.hex()
        # Engine speed, then the coolant, 1 degC per bit from -40: over the threshold when it set.
        assert int.from_bytes(frozen[5:7], "little") / 8 > IDLE_RPM and frozen[7] - 40 >= HOT_ON_C, frozen.hex()

        # DM11 to the node: acknowledged, to everyone, naming us.
        request(bus, PGN_DM11, da=sa)
        ack = first(bus, is_from(PGN_ACK, sa))[-1]
        assert parse(ack)[1] == GLOBAL and bytes(ack.data) == bytes([ACK, 0xFF, 0xFF, 0xFF, TESTER]) + PGN_DM11.to_bytes(3, "little"), bytes(ack.data).hex()
        # Still hot: the model raises both again, their counts restarted.
        assert dtcs_of(ask(bus, sa, PGN_DM1)) == {(*DTC_COOLANT, 1), (*DTC_OIL, 1)}

        # Cooled, both are history again, as the other tests find them.
        set_mode(CHARGE)
        first(bus, no_dtc, 2 * VCU_CYCLE_S + bench.WAIT_S)
        assert {d[:2] for d in dtcs_of(ask(bus, sa, PGN_DM2))} == both


@pytest.mark.parametrize("spn, fmi, limits", [(110, 0, (HOT_ON_C + 40, 0xFFFF)), (100, 1, (0xFFFF, 200 // 4)), (190, 31, (0xFFFF, 0xFFFF))], ids=["coolant", "oil-pressure", "no-such-test"])
def test_dm7_commands_a_test_answered_by_dm30(bus, node, spn, fmi, limits):
    """DM7 for an SPN's tests gets DM30 by RTS/CTS: the value as the SPN's PG carries it,
    against the limit its DTC sets at; an SPN without a test cannot be performed."""
    sa, _ = node
    result = answer(bus, sa, PGN_DM30, pdu(PGN_DM7, bytes([247, spn & 0xFF, spn >> 8, 31, 0xFF, 0xFF, 0xFF, 0xFF]), sa))
    assert len(result) == 12 and result[0] == 247 and (result[1] | result[2] << 8, result[3]) == (spn, fmi), result.hex()
    value, high, low = (int.from_bytes(result[i : i + 2], "little") for i in (6, 8, 10))
    # 0xFB01: cannot be performed, as oil pressure with the engine below idle; then no limits.
    if value == 0xFB01:
        assert spn != 110 and (high, low) == (0xFFFF, 0xFFFF), result.hex()
    else:
        assert spn != 190 and value <= 250 and (high, low) == limits, result.hex()


PGN_DM4, PGN_DM35, PGN_DM42, PGN_DM58 = 65229, 40704, 64862, 64475


def test_dm35_dm42_dm4_and_dm58(bus, node):
    """DM35 and DM42 list what DM1 lists. DM4 is one freeze frame: the bytes that follow,
    a DTC, 8 data bytes. DM7 with the rationality test identifier gets DM58: the SPN's data
    as its PG carries it."""
    sa, _ = node
    active = dtcs_of(ask(bus, sa, PGN_DM1))
    assert dtcs_of(ask(bus, sa, PGN_DM35)) == active == dtcs_of(ask(bus, sa, PGN_DM42))

    frame = ask(bus, sa, PGN_DM4)
    assert len(frame) == 13 and frame[0] == 12, frame.hex()
    assert {d[:2] for d in dtcs_of(bytes(2) + frame[1:5])} <= {DTC_COOLANT, DTC_OIL}, frame.hex()
    assert 0 < int.from_bytes(frame[5:7], "little") / 8 <= 2000 and frame[10:] == b"\xff" * 3, frame.hex()

    speed = answer(bus, sa, PGN_DM58, pdu(PGN_DM7, bytes([245, 190, 0, 0x1F, 0xFF, 0xFF, 0xFF, 0xFF]), sa))
    assert speed[:4] == bytes([245, 190, 0, 0x1F]) and speed[6:] == b"\xff\xff", speed.hex()
    assert 0 <= int.from_bytes(speed[4:6], "little") / 8 <= 2000, speed.hex()
    none = answer(bus, sa, PGN_DM58, pdu(PGN_DM7, bytes([245, 92, 0, 0x1F, 0xFF, 0xFF, 0xFF, 0xFF]), sa))
    assert none == bytes([245, 92, 0, 0x1F]) + b"\xff" * 4, none.hex()


def dm14(sa, command, pointer, length, key):
    data = DM14.encode({"Length": length & 0xFF, "LengthHigh": length >> 8, "Command": command, "PointerType": 0, "Pointer": pointer, "PointerExtension": 0, "KeyOrUserLevel": key})
    # Byte 2 bit 1 is sent as one.
    return pdu(PGN_DM14, bytes([data[0], data[1] | 1, *data[2:]]), sa)


def dm15(bus, sa, to=None):
    return DM15.decode(answer(bus, sa, PGN_DM15, to), decode_choices=False)


def unlock(bus, sa, command, pointer, length):
    """DM14 twice: a user level gets a seed and nothing granted, the key gets the access. The granting DM15."""
    challenge = dm15(bus, sa, dm14(sa, command, pointer, length, 7))
    assert (challenge["Status"], challenge["LengthAllowed"]) == (MEM_PROCEED, 0) and challenge["Seed"] not in (0, NO_KEY), challenge
    granted = dm15(bus, sa, dm14(sa, command, pointer, length, challenge["Seed"] ^ MEM_KEY))
    assert (granted["Status"], granted["LengthAllowed"], granted["Seed"]) == (MEM_PROCEED, length, NO_KEY), granted
    return granted


def read_memory(bus, sa, pointer, length):
    unlock(bus, sa, MEM_READ, pointer, length)
    data = answer(bus, sa, PGN_DM16)
    assert dm15(bus, sa)["Status"] == MEM_COMPLETED
    bus.send(dm14(sa, MEM_COMPLETED, pointer, length, NO_KEY))
    # The count, or 0xFF above 7 bytes, where DM16 goes by transport; a frame is padded.
    assert data[0] == (length if length <= 7 else 0xFF) and len(data) == max(1 + length, 8), data.hex()
    return data[1 : 1 + length]


def write_memory(bus, sa, pointer, data):
    unlock(bus, sa, MEM_WRITE, pointer, len(data))
    assert dm15(bus, sa, pdu(PGN_DM16, bytes([len(data), *data]), sa, prio=7))["Status"] == MEM_COMPLETED
    bus.send(dm14(sa, MEM_COMPLETED, pointer, len(data), NO_KEY))


def test_memory_access_by_seed_and_key(bus, node):
    """DM14 to DM16: no access without the key; with it a read in a frame, a
    longer one by RTS/CTS, and a write that reads back. The memory is left as
    it began."""
    sa, _ = node
    # An earlier access of ours, left open, is closed.
    bus.send(dm14(sa, MEM_COMPLETED, 0, 1, NO_KEY))

    assert read_memory(bus, sa, 0x10, 4) == MEMORY[0x10:0x14]
    assert read_memory(bus, sa, 0, 16) == MEMORY[:16]

    write_memory(bus, sa, 0x20, b"\x01\x02\x03\x04")
    try:
        assert read_memory(bus, sa, 0x1E, 7) == MEMORY[0x1E:0x20] + b"\x01\x02\x03\x04" + MEMORY[0x24:0x25]
    finally:
        write_memory(bus, sa, 0x20, MEMORY[0x20:0x24])
    assert read_memory(bus, sa, 0x20, 4) == MEMORY[0x20:0x24]


@pytest.mark.parametrize(
    "command, pointer, length, error",
    [(MEM_READ, 64, 1, 0x100), (MEM_READ, 0, 0, 0x102), (MEM_READ, 60, 5, 0x102), (MEM_WRITE, 0, 8, 0x102), (0, 0, 1, 0x1), (MEM_READ, 0, 4, 0x1003)],
    ids=["pointer-outside", "no-length", "past-the-end", "write-over-a-frame", "erase-unsupported", "wrong-key"],
)
def test_memory_access_refused(bus, node, command, pointer, length, error):
    """A request the node cannot grant fails with DM15: operation failed, the error indicator and EDCP extension 7."""
    sa, _ = node
    bus.send(dm14(sa, MEM_COMPLETED, 0, 1, NO_KEY))
    reply = dm15(bus, sa, dm14(sa, command, pointer, length, 7))
    if error == 0x1003:
        # The same request again with a key that is not the seed's.
        reply = dm15(bus, sa, dm14(sa, command, pointer, length, reply["Seed"] ^ MEM_KEY ^ 1))
    assert (reply["Status"], reply["LengthAllowed"], reply["EDCParameter"], reply["EDCPExtension"], reply["Seed"]) == (MEM_FAILED, 0, error, 7, NO_KEY), reply


PGN_DM13 = 57088
# From nodes/genset: a stop holds this long past the last one.
DM13_HOLD_S = 6.0
DM13_STOP, DM13_START = bytes([0xFC]) + b"\xff" * 7, bytes([0xFD]) + b"\xff" * 7


def periodic_from(frames, sa):
    """The node's periodic PGs among frames."""
    pgns = {pgn_of(m.frame_id) for m in PERIODIC}
    return [m for m in frames if theirs(m) and parse(m)[2] == sa and parse(m)[0] in pgns]


@pytest.mark.parametrize("da", ["node", "global"])
def test_dm13_stops_and_starts_the_broadcasts(bus, node, da):
    """DM13 stop, to the node or to everyone, holds every periodic PG while requests
    are still answered; DM13 start brings them back at once."""
    sa, _ = node
    to = sa if da == "node" else GLOBAL
    try:
        send(bus, 6, PGN_DM13, DM13_STOP, da=to)
        # What was already on its way, then silence well inside the hold.
        collect(bus, 0.5)
        assert periodic_from(collect(bus, 2.0), sa) == []
        request(bus, PGN_HOURS, da=sa)
        first(bus, is_from(PGN_HOURS, sa))
    finally:
        send(bus, 6, PGN_DM13, DM13_START, da=to)
    got = window(bus, sa, 10)
    assert {parse(m)[0] for m in periodic_from(got, sa)} >= {PGN_EEC1, PGN_EEC2}


def test_dm13_stop_ends_by_itself(bus, node):
    """A stop that is not renewed ends after the hold, and one that is, holds on."""
    sa, _ = node
    send(bus, 6, PGN_DM13, DM13_STOP, da=sa)
    collect(bus, 0.5)
    try:
        # Renewed every 2 s, quiet past one hold.
        for _ in range(4):
            assert periodic_from(collect(bus, 2.0), sa) == []
            send(bus, 6, PGN_DM13, DM13_STOP, da=sa)
        first(bus, is_from(PGN_EEC1, sa), DM13_HOLD_S + bench.WAIT_S)
    finally:
        send(bus, 6, PGN_DM13, DM13_START, da=sa)

