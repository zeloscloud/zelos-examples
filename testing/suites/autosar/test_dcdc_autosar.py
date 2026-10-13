"""The DC-DC as an AUTOSAR ECU, tested the way a bench tests one: from its system description.

arxml/can-full.arxml is the only source of ids, layouts, scaling, timing, E2E
and SecOC parameters and of the UDS addressing. cantools loads it; the suite
reads the bus. Values are cross-checked against DCDC_Status decoded with the
bench DBC: every AUTOSAR frame carries one model sample, as a DCDC_Status does.

The suite plays the BMS (an allowance and a pack voltage) unless one is on the
bus, so the node converts and every value is live. The bus is recorded by
`candump` (can-utils), which the host needs. Needs the `full` image (or
`dcdc -S autosar -S uds`) on a SocketCAN channel.

    cd testing && uv run pytest suites/autosar --channel can0
"""

import contextlib
import time
from itertools import pairwise

import can
import isotp
import pytest
from cantools.autosar.end_to_end import check_profile2_crc, check_profile5_crc
from cantools.autosar.secoc import verify_authenticator
from udsoncan.client import Client
from udsoncan.configs import default_client_config
from udsoncan.connections import PythonIsoTpConnection
from udsoncan.exceptions import NegativeResponseException

from zelos_testing import arxml, dbc, uds_keys
from zelos_testing.dcdc import LIMITS, LIMITS_PERIOD_S, STATUS, BmsLimits
from zelos_testing.frames import assert_period, bound, collect, count, play_unless_present, recording, until
from zelos_testing.uds import DIDS

DB = arxml.load()
BENCH = dbc.load("bench")
BMS_STATUS = BENCH.get_message_by_name("BMS_Status")
P1, P2, P5 = (DB.get_message_by_name(n) for n in ("DcdcOutputP1", "DcdcThermalP2", "DcdcPowerP5"))
CONTAINER, MUX, SECURED = (DB.get_message_by_name(n) for n in ("DcdcContainer", "DcdcMux", "DcdcSecured"))
# The DC-DC's frames the description adds to bench.dbc.
AUTOSAR = [m for m in DB.messages if "DCDC" in m.senders and m.frame_id not in {b.frame_id for b in BENCH.messages}]
SELECTOR = next(s.name for s in MUX.signals if s.is_multiplexer)
PACK_V = 374.0

pytestmark = pytest.mark.usefixtures("bus_health")


@pytest.fixture(scope="module")
def socketcan(pytestconfig):
    if pytestconfig.getoption("interface") != "socketcan":
        pytest.skip("records the bus with candump: a SocketCAN channel only")


class BmsStatus:
    """BMS_Status as a BMS sends it, a steady pack. Call for the next frame."""

    def __init__(self):
        self.counter = 0

    def __call__(self) -> can.Message:
        data = BMS_STATUS.encode({"PackVoltage": PACK_V, "PackCurrent": 5.0, "StateOfCharge": 80.0, "ContactorState": 2, "Counter": self.counter})
        self.counter = (self.counter + 1) & 0xFF
        return can.Message(arbitration_id=BMS_STATUS.frame_id, data=data, is_extended_id=False)


@pytest.fixture(scope="module")
def capture(pytestconfig, socketcan) -> list[can.Message]:
    """Two seconds of the node's time once its draw settles: its frames and the BMS's, as candump recorded them."""
    channel = pytestconfig.getoption("channel")
    ids = [STATUS.frame_id, LIMITS.frame_id, BMS_STATUS.frame_id, *(m.frame_id for m in AUTOSAR)]
    with can.Bus(interface="socketcan", channel=channel) as bus:
        if not collect(bus, bound(0.5), ids=[STATUS.frame_id]):
            pytest.skip("no DCDC_Status on the bus: not the DC-DC node")

        def window(f):
            return count(f, STATUS.frame_id) >= 20

        with (
            play_unless_present(bus, LIMITS.frame_id, BmsLimits(aux=10.0), LIMITS_PERIOD_S, window),
            play_unless_present(bus, BMS_STATUS.frame_id, BmsStatus(), BMS_STATUS.cycle_time / 1000, window),
        ):
            # Settled: three DCDC_Status in a row draw the same, nonzero current.
            until(bus, lambda f: len(drawn := [STATUS.decode(m.data)["InputCurrent"] for m in f[-3:]]) == 3 and drawn[0] > 0 and len(set(drawn)) == 1, ids=[STATUS.frame_id])
            # This process's socket only paces the record: the node's own DCDC_Status are its clock.
            with recording(channel, ids) as recorded:
                until(bus, lambda f: count(f, STATUS.frame_id) >= 41, ids=[STATUS.frame_id])
    return recorded


def of(capture, message) -> list[can.Message]:
    return [m for m in capture if m.arbitration_id == message.frame_id]


def gaps(msgs: list[can.Message]) -> list[float]:
    return [b.timestamp - a.timestamp for a, b in pairwise(msgs)]


def counts_by_one(msgs: list[can.Message], counter, modulo: int) -> set[int]:
    """Counter steps between consecutive frames; a record without loss has only steps of 1."""
    return {(counter(b) - counter(a)) % modulo for a, b in pairwise(msgs)}


def raw(message, msg: can.Message) -> dict:
    return message.decode(msg.data, decode_choices=False, scaling=False)


# --- The description ------------------------------------------------------------


def test_description_carries_bench_dbc_unchanged():
    # A tester can switch from the DBC to the ARXML and decode the bench alike.
    for m in BENCH.messages:
        a = DB.get_message_by_frame_id(m.frame_id)
        assert (a.name, a.length, a.cycle_time, a.senders, a.is_extended_frame) == (m.name, m.length, m.cycle_time, m.senders, m.is_extended_frame)
        for s in m.signals:
            t = a.get_signal_by_name(s.name)
            assert (t.start, t.length, t.byte_order, t.is_signed, t.scale, t.offset, t.unit or None, sorted(t.receivers)) == (s.start, s.length, s.byte_order, s.is_signed, s.scale, s.offset, s.unit or None, sorted(s.receivers)), f"{m.name}.{s.name}"
            assert {k: str(v) for k, v in (t.choices or {}).items()} == {k: str(v) for k, v in (s.choices or {}).items()}, f"{m.name}.{s.name}"
            if not t.choices:  # an ARXML TEXTTABLE carries no range; the DBC's is its values'
                assert (t.minimum, t.maximum) == pytest.approx((s.minimum, s.maximum)), f"{m.name}.{s.name}"


# --- On the bus -------------------------------------------------------------------


def test_frames_keep_the_described_ids_lengths_and_cycles(capture):
    for m in AUTOSAR:
        got = of(capture, m)
        assert got, f"no {m.name} (0x{m.frame_id:03X}): is the image built with -S autosar?"
        assert {len(f.data) for f in got} == {m.length}, m.name
    for m in (P1, P2, P5, SECURED):
        assert_period(gaps(of(capture, m)), m.cycle_time / 1000, 0.02)
    # A contained PDU and a dynamic part each keep their own cycle.
    for c in CONTAINER.contained_messages:
        assert_period(gaps([f for f in of(capture, CONTAINER) if int.from_bytes(f.data[:3]) == c.header_id]), c.cycle_time / 1000, 0.02)
    for code in (0, 1):
        assert_period(gaps([f for f in of(capture, MUX) if raw(MUX, f)[SELECTOR] == code]), MUX.cycle_time / 1000, 0.02)


def test_e2e_profile_1(capture):
    cfg = arxml.e2e_p01(P1.name)
    data = [f.data for f in of(capture, P1)]
    for d in data:
        assert d[cfg.crc_offset // 8] == arxml.p01_crc(cfg, d), f"wrong CRC: {d.hex()}"
    counters = [raw(P1, f)["Counter"] for f in of(capture, P1)]
    steps = counts_by_one(of(capture, P1), lambda f: raw(P1, f)["Counter"], 15)
    assert steps == {1} and 15 not in counters, f"counters {counters}"
    flipped = bytearray(data[0])
    flipped[4] ^= 0x01
    assert flipped[0] != arxml.p01_crc(cfg, flipped), "a flipped bit must fail the check"


def test_e2e_profile_2(capture):
    data = [f.data for f in of(capture, P2)]
    assert all(check_profile2_crc(d, P2) for d in data)
    steps = counts_by_one(of(capture, P2), lambda f: f.data[1] & 0x0F, 16)
    assert steps == {1}, f"counters {[d[1] & 0x0F for d in data]}"
    flipped = bytearray(data[0])
    flipped[3] ^= 0x01
    assert not check_profile2_crc(flipped, P2)


def test_e2e_profile_5(capture):
    data = [f.data for f in of(capture, P5)]
    assert all(check_profile5_crc(d, P5) for d in data)
    steps = counts_by_one(of(capture, P5), lambda f: f.data[2], 256)
    assert steps == {1}, f"counters {[d[2] for d in data]}"
    flipped = bytearray(data[0])
    flipped[5] ^= 0x01
    assert not check_profile5_crc(flipped, P5)


def test_container_carries_its_pdus_in_turn(capture):
    ids = [c.header_id for c in CONTAINER.contained_messages]
    contained = [DB.decode_message(CONTAINER.frame_id, f.data, decode_containers=True) for f in of(capture, CONTAINER)]
    assert all(len(c) == 1 for c in contained), "one contained PDU per classic frame"
    turn = counts_by_one(of(capture, CONTAINER), lambda f: ids.index(int.from_bytes(f.data[:3])), len(ids))
    assert turn == {1}, f"header ids {[hex(c[0][0].header_id) for c in contained]}"


def test_multiplexed_alternates_its_dynamic_part(capture):
    decoded = [MUX.decode(f.data, decode_choices=False) for f in of(capture, MUX)]
    assert counts_by_one(of(capture, MUX), lambda f: raw(MUX, f)[SELECTOR], 2) == {1}, "selector alternates"
    for d in decoded:
        assert {"DcdcState", "OutputVoltage"} <= d.keys(), "the static part is in every frame"
        assert {"OutputCurrent", "InputCurrent"} <= d.keys() if d[SELECTOR] == 0 else {"AuxCurrentLimit", "SetpointVoltage"} <= d.keys()


def test_secured_pdu_authenticates(capture):
    sec = SECURED.autosar.secoc
    data = [f.data for f in of(capture, SECURED)]
    # The whole freshness value is sent (8 of 8 bits): each frame verifies on its own.
    freshness = [d[sec.payload_length] for d in data]
    for d, fv in zip(data, freshness, strict=True):
        assert verify_authenticator(d, SECURED, arxml.cmac(arxml.SECOC_KEY), fv), f"MAC: {d.hex()}"
    steps = counts_by_one(of(capture, SECURED), lambda f: f.data[sec.payload_length], 256)
    assert steps == {1}, f"freshness {freshness}"
    assert not verify_authenticator(data[0], SECURED, arxml.cmac(bytes(16)), freshness[0]), "another key must fail"


def decoded_values(capture) -> list[tuple[can.Message, object, dict]]:
    """Every AUTOSAR frame's physical values, by PDU: contained PDUs unpacked, the trailer stripped."""
    out = []
    for f in capture:
        if f.arbitration_id == CONTAINER.frame_id:
            out += [(f, c, values) for c, values in DB.decode_message(f.arbitration_id, f.data, decode_containers=True, decode_choices=False)]
        elif f.arbitration_id in {m.frame_id for m in AUTOSAR}:
            out.append((f, DB.get_message_by_frame_id(f.arbitration_id), DB.decode_message(f.arbitration_id, f.data, decode_choices=False)))
    return out


def test_values_match_dcdc_status(capture):
    # Each frame carries one model sample: the latest DCDC_Status's, or the next one's,
    # to a step of the coarser resolution (the DBC codec truncates, COM rounds).
    status = [(f.timestamp, STATUS.decode(f.data, decode_choices=False)) for f in of(capture, STATUS)]
    shared = {s.name for s in STATUS.signals} - {"Counter"}
    checked = 0
    for f, pdu, values in decoded_values(capture):
        neighbours = [s for t, s in status if t <= f.timestamp][-1:] + [s for t, s in status if t > f.timestamp][:1]
        for name in shared & values.keys():
            res = max(STATUS.get_signal_by_name(name).scale, pdu.get_signal_by_name(name).scale)
            assert any(abs(values[name] - s[name]) <= res + 1e-6 for s in neighbours), f"{pdu.name}.{name} {values[name]} vs {[s[name] for s in neighbours]}"
            checked += 1
        if "OutputPower" in values:  # V x A, each to its DBC resolution, then 1 W
            assert any(abs(values["OutputPower"] - s["OutputVoltage"] * s["OutputCurrent"]) < 2.0 for s in neighbours)
        if "DcdcState" in values:  # Converting: drawing, below the derate temperature
            assert any(values["DcdcState"] == (2 if s["DerateActive"] else 1 if s["InputCurrent"] > 0 else 0) for s in neighbours)
    assert checked > 200


def test_values_follow_the_bms(capture):
    # From the second BMS frame of each kind on: the node may hold one from before the capture.
    pack, limits = of(capture, BMS_STATUS), of(capture, LIMITS)
    voltages = {round(BMS_STATUS.decode(f.data)["PackVoltage"], 1) for f in pack}
    allowances = {round(LIMITS.decode(f.data)["AuxCurrentLimit"], 2) for f in limits}
    for f, pdu, values in decoded_values(capture):
        if "InputVoltage" in values and f.timestamp > pack[1].timestamp:
            assert round(values["InputVoltage"], 1) in voltages, f"{pdu.name}.InputVoltage {values['InputVoltage']}"
        if "AuxCurrentLimit" in values and f.timestamp > limits[1].timestamp:
            assert round(values["AuxCurrentLimit"], 2) in allowances, f"{pdu.name}.AuxCurrentLimit {values['AuxCurrentLimit']}"


# --- UDS, addressed from the description -----------------------------------------


@contextlib.contextmanager
def uds_tester(pytestconfig, txid: int | None = None):
    """A udsoncan client on the CanTp connection the ARXML describes."""
    tp = arxml.cantp()
    config = {"interface": pytestconfig.getoption("interface"), "channel": pytestconfig.getoption("channel")}
    with can.Bus(**config) as bus:
        notifier = can.Notifier(bus, [])
        params = {"blocksize": tp.block_size, "stmin": 0, "blocking_send": True}
        if tp.padding:
            params |= {"tx_padding": 0xCC, "tx_data_min_length": 8}
        stack = isotp.NotifierBasedCanStack(bus, notifier, address=isotp.Address(isotp.AddressingMode.Normal_11bits, txid=txid or tp.physical, rxid=tp.response), params=params)
        cfg = dict(default_client_config) | {"request_timeout": None, "p2_timeout": bound(2.0), "p2_star_timeout": bound(5.0), "use_server_timing": False, "data_identifiers": DIDS, "security_algo": lambda level, seed, params=None: uds_keys.key(level, seed)}
        try:
            with Client(PythonIsoTpConnection(stack), config=cfg) as client:
                yield client
        finally:
            notifier.stop()


def test_uds_answers_on_the_described_connections(pytestconfig, socketcan, capture):
    tp = arxml.cantp()
    with recording(pytestconfig.getoption("channel"), [tp.response]) as recorded:
        with uds_tester(pytestconfig) as client:
            # Four identifiers: a multi-frame request and a multi-frame response.
            dids = client.read_data_by_identifier([0xF190, 0xF187, 0xF18A, 0xF18C]).service_data.values
            assert dids[0xF190] == "1ZLSDCDC0BENCH001"
            setpoint = client.read_data_by_identifier(0x0200).service_data.values[0x0200]
        with uds_tester(pytestconfig, txid=tp.functional) as functional:
            functional.conn.empty_rxqueue()
            functional.conn.send(b"\x3e\x00")
            assert functional.conn.wait_frame(timeout=bound(1.0)) == b"\x7e\x00", "functional request answered on the response id"
    # The server's flow control is what the description says: block size, STmin, padding.
    flow = [f.data for f in recorded if f.data[0] >> 4 == 3]
    assert flow, "no flow control from the ECU"
    assert all((f[1], f[2]) == (tp.block_size, round(tp.st_min * 1000)) for f in flow), [f.hex() for f in flow]
    assert all(len(f) == 8 for f in recorded) if tp.padding else True
    # One setpoint, two protocols.
    mux_limits = [values for _, pdu, values in decoded_values(capture) if pdu is MUX and "SetpointVoltage" in values]
    assert mux_limits and all(v["SetpointVoltage"] == pytest.approx(setpoint, abs=0.005) for v in mux_limits)


def test_communication_control_silences_the_pdus(pytestconfig, socketcan):
    # 0x28 disables the node's normal messages, these among them. E2E counters
    # count frames sent: a receiver sees the silence as a timeout, not lost data.
    with recording(pytestconfig.getoption("channel"), [m.frame_id for m in AUTOSAR]) as recorded, uds_tester(pytestconfig) as client:
        try:
            client.change_session(0x03)
            end = time.monotonic() + bound(11.0)  # a power-up security delay
            while True:
                try:
                    client.unlock_security_access(0x01)
                    break
                except NegativeResponseException as e:  # 0x37 until the delay ends
                    if e.response.code != 0x37 or time.monotonic() > end:
                        raise
                    time.sleep(0.5)
            client.communication_control(0x01, 0x01)
            off = time.time()
            time.sleep(bound(0.5))
            client.communication_control(0x00, 0x01)
            on = time.time()
        finally:
            client.change_session(0x01)
        # The node's time: five P05 cycles after the PDUs resume.
        time.sleep(bound(5 * P5.cycle_time / 1000))
    silent = [m for m in recorded if off + bound(0.1) < m.timestamp < on]
    assert silent == [], f"{len(silent)} AUTOSAR frames while transmission was disabled"
    p5 = [m for m in recorded if m.arbitration_id == P5.frame_id]
    assert sum(m.timestamp > on for m in p5) >= 3, "the PDUs resume"
    assert counts_by_one(p5, lambda f: f.data[2], 256) == {1}, f"P05 counters {[m.data[2] for m in p5]}"
    assert any(b.timestamp - a.timestamp > 0.3 for a, b in pairwise(p5)), "a silence"
