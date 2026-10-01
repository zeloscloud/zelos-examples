"""The DC-DC node's XCP slave, driven by pyxcp as an independent master.

Needs the `-S xcp` build and the A2L generated beside its ELF (--a2l). The suite
plays the BMS unless a real one is on the bus. Every test connects afresh and
sets what it relies on, and a calibration test restores the value it found.

The DAQ tests record what the node sends into the test's Zelos trace, decoded
with the bench DBC, and assert with `check`, so the verdicts sit beside the
data: `--zelos-trace-file --zelos-local-artifacts-dir out` writes both, and on
a bench with an agent they also stream to it.
"""

import contextlib
import logging
import math
import os
import re
import struct
import time
from pathlib import Path

import can
import pytest
import zelos_sdk
from zelos_can import CanDecoder

from zelos_testing.dbc import DBC_DIR
from zelos_testing.dcdc import LIMITS, LIMITS_PERIOD_S, PERIOD_S, STATUS, BmsLimits, cycles, input_current, settle
from zelos_testing.frames import assert_period, bound, count, drain, play_unless_present, until
from zelos_testing.xcp import TIMESTAMP_TICK_S, addresses, epk, odt, read, ticks, timestamp

# pyxcp stamps its session with the local zone and fails on abbreviations
# such as PST, so it runs in UTC.
os.environ["TZ"] = "UTC"
time.tzset()

from pyxcp.config import create_application_from_config, set_application  # noqa: E402
from pyxcp.master import Master  # noqa: E402
from pyxcp.types import XcpResponseError  # noqa: E402

CRO, DTO = 0x6F0, 0x6F1
ERR_DAQ_ACTIVE = 0x11
ERR_WRITE_PROTECTED = 0x23
ERR_ACCESS_DENIED = 0x24
ERR_MODE_NOT_VALID = 0x27
EV_DAQ_OVERLOAD = bytes([0xFD, 0x06])
# SET_DAQ_LIST_MODE bits.
DAQ_TIMESTAMP, DAQ_STIM = 0x10, 0x02

# The allowance this suite sends when it plays the BMS.
AUX_A = 10.0
# A demand below every allowance either BMS grants (a real one: 2 to 6 A by
# mode), so the current it settles at does not depend on which BMS is present.
LOW_A = 1.5

pytestmark = pytest.mark.usefixtures("bus_health")


@pytest.fixture(scope="module")
def a2l_path(pytestconfig):
    """The A2L generated beside the ELF.

    --a2l, or the one in /elf, where a bench's tester mounts its ELFs.
    """
    path = pytestconfig.getoption("a2l")
    if path is None:
        found = sorted(Path("/elf").glob("*.a2l"))
        if len(found) != 1:
            pytest.fail(f"{len(found)} A2Ls in /elf: pass --a2l with the one built beside the ELF")
        path = found[0]
    return path


@pytest.fixture(scope="module")
def a2l_text(a2l_path):
    return read(a2l_path)


@pytest.fixture(scope="module")
def a2l(a2l_text):
    """Object name -> address."""
    return addresses(a2l_text)


@pytest.fixture
def xcp(pytestconfig):
    app = create_application_from_config(
        {
            "Transport": {
                "timeout": bound(2.0),
                "CAN": {
                    "interface": "socketcan",
                    "channel": pytestconfig.getoption("channel"),
                    "can_id_master": CRO,
                    "can_id_slave": DTO,
                }
            },
            # Raise XCP errors to the test, rather than retrying or exiting.
            "General": {"disable_error_handling": True},
        },
        log_level=logging.WARNING,
    )
    set_application(app)
    with Master("can", config=app) as master:
        master.connect()
        yield master
        # Leave no DAQ running for whoever connects next.
        master.freeDaq()
        master.disconnect()


@pytest.fixture
def bms(bus):
    """Plays the BMS with a generous allowance, unless a real BMS is on the bus."""
    # A second of the node's time sees ten from a BMS.
    with play_unless_present(bus, LIMITS.frame_id, BmsLimits(aux=AUX_A), LIMITS_PERIOD_S, lambda frames: count(frames, STATUS.frame_id) >= 20):
        yield


@pytest.fixture
def recorded(pytestconfig):
    """Everything on the bus while the test runs, decoded with the bench DBC into its trace.

    A second receiver, so the test's own reads are untouched. XCP is not in the
    DBC: its frames are recorded raw.
    """
    decoder = CanDecoder(database_file=str(DBC_DIR / "bench.dbc"), source_name="bus", log_raw_frames=True)
    with can.Bus(interface="socketcan", channel=pytestconfig.getoption("channel")) as bus:
        notifier = can.Notifier(bus, [decoder.decode_message])
        try:
            yield
        finally:
            notifier.stop()


def read_f32(xcp, addr):
    return struct.unpack("<f", xcp.shortUpload(4, addr))[0]


def write_f32(xcp, addr, value):
    xcp.setMta(addr)
    xcp.download(struct.pack("<f", value))


class calibrated:
    """Characteristics calibrated to these values for the block, then restored."""

    def __init__(self, xcp, a2l, **values):
        self.xcp, self.values = xcp, {a2l[name]: value for name, value in values.items()}

    def __enter__(self):
        self.found = {addr: read_f32(self.xcp, addr) for addr in self.values}
        for addr, value in self.values.items():
            write_f32(self.xcp, addr, value)
        return self

    def __exit__(self, *exc):
        for addr, value in self.found.items():
            if exc[0] is None:
                write_f32(self.xcp, addr, value)
                assert read_f32(self.xcp, addr) == value
            else:
                # The block's failure is the one to report.
                with contextlib.suppress(Exception):
                    write_f32(self.xcp, addr, value)


def next_allowance(bus):
    """AuxCurrentLimit of the next BMS_Limits on the bus."""
    frames = until(bus, lambda frames: True, ids=[LIMITS.frame_id])
    return LIMITS.decode(frames[0].data)["AuxCurrentLimit"]


def daq_list(xcp, a2l, names, mode=0):
    """One list on event 0 (the 50 ms cycle), one float per ODT, selected. Returns its first PID.

    With a timestamp, the first ODT carries only that: the 3 bytes it leaves do not fit a float.
    """
    odts = [[]] * bool(mode & DAQ_TIMESTAMP) + [[name] for name in names]
    xcp.freeDaq()
    xcp.allocDaq(1)
    xcp.allocOdt(0, len(odts))
    for i, entries in enumerate(odts):
        xcp.allocOdtEntry(0, i, len(entries))
    for i, entries in enumerate(odts):
        for name in entries:
            xcp.setDaqPtr(0, i, 0)
            xcp.writeDaq(0xFF, 4, 0, a2l[name])
    xcp.setDaqListMode(mode, 0, 0, 1, 0)
    return xcp.startStopDaqList(2, 0).firstPid


def connected(frames):
    """Index of the first positive CONNECT response among frames, or None."""
    return next((i for i, m in enumerate(frames) if m.arbitration_id == DTO and m.data[0] == 0xFF and len(m.data) == 8), None)


def error(call):
    """The XCP error code call is answered with, or None."""
    try:
        call()
    except XcpResponseError as e:
        return int(e.get_error_code())
    return None


def test_connect(xcp):
    props = xcp.slaveProperties
    assert (props.maxCto, props.maxDto, str(props.byteOrder)) == (8, 8, "INTEL")
    assert xcp.identifier(1) == "dcdc"


def test_epk(xcp, a2l_text, check):
    # The A2L is this build's: by GET_ID and at ADDR_EPK, as a tool checks it.
    text, addr = epk(a2l_text)
    check.that(xcp.identifier(5), "==", text, name="GET_ID 5 (EPK) is the A2L's EPK")
    xcp.setMta(addr)
    check.that(xcp.fetch(len(text)).decode(), "==", text, name="EPK read at ADDR_EPK")


def test_measurements(bus, bms, xcp, a2l):
    slew = read_f32(xcp, a2l["SLEW_A_PER_S"])
    with calibrated(xcp, a2l, DEMAND_A=LOW_A):
        settle(bus, LOW_A)
        # A real BMS changes the allowance with the VCU's mode, so the node
        # holds the allowance of a frame just before or just after the read.
        before = next_allowance(bus)
        aux = read_f32(xcp, a2l["aux_limit_a"])
        assert aux == pytest.approx(before, abs=0.01) or aux == pytest.approx(next_allowance(bus), abs=0.01)
        assert read_f32(xcp, a2l["setpoint_a"]) == pytest.approx(LOW_A)
        assert read_f32(xcp, a2l["input_current_a"]) == pytest.approx(LOW_A, abs=0.01)
    assert read_f32(xcp, a2l["slew_step_a"]) == pytest.approx(slew * PERIOD_S)
    on_bus = STATUS.decode(cycles(bus, 1)[-1].data)["Temperature"]
    assert read_f32(xcp, a2l["temperature_c"]) == pytest.approx(on_bus, abs=1.0)


def test_only_registered_memory(xcp, a2l):
    # A measurement is readable, not writable.
    xcp.setMta(a2l["input_current_a"])
    with pytest.raises(XcpResponseError) as e:
        xcp.download(b"\0\0\0\0")
    assert int(e.value.get_error_code()) == ERR_WRITE_PROTECTED

    # Nothing outside the registered variables: not flash, not a read that
    # straddles a variable's end, not the word after the last one.
    for addr in (0x08000000, a2l["DEMAND_A"] + 2, max(a2l.values()) + 4):
        with pytest.raises(XcpResponseError) as e:
            xcp.shortUpload(4, addr)
        assert int(e.value.get_error_code()) == ERR_ACCESS_DENIED, hex(addr)


def test_calibration_is_live(bus, bms, xcp, a2l):
    # Down, then up, so the current is seen to follow the calibration both ways.
    with calibrated(xcp, a2l, DEMAND_A=LOW_A / 2):
        settle(bus, LOW_A / 2)
        write_f32(xcp, a2l["DEMAND_A"], LOW_A)
        assert read_f32(xcp, a2l["DEMAND_A"]) == LOW_A
        settle(bus, LOW_A)


def test_calibration_is_bounded(bus, bms, xcp, a2l):
    # The node bounds what it is sent to the A2L's limits: past the top to the
    # upper bound, NaN to the lower. Measured two cycles on, once it has run.
    with calibrated(xcp, a2l, DEMAND_A=1000.0, SLEW_A_PER_S=1000.0):
        cycles(bus, 2)
        assert read_f32(xcp, a2l["setpoint_a"]) <= 20.0
        assert read_f32(xcp, a2l["slew_step_a"]) == pytest.approx(100.0 * PERIOD_S)
    with calibrated(xcp, a2l, DEMAND_A=math.nan, SLEW_A_PER_S=math.nan):
        cycles(bus, 2)
        assert read_f32(xcp, a2l["setpoint_a"]) == 0.0
        assert read_f32(xcp, a2l["slew_step_a"]) == pytest.approx(0.1 * PERIOD_S)


def test_daq_on_cycle_event(bus, bms, xcp, a2l):
    # Two ODTs of one float each.
    first_pid = daq_list(xcp, a2l, ["input_current_a", "temperature_c"])
    xcp.startStopSynch(1)

    # Counted in the node's cycles, not in host seconds, which on Renode
    # measure how fast the simulation runs.
    frames = cycles(bus, 40, [DTO])
    xcp.startStopSynch(0)
    stopped = time.time()
    after = cycles(bus, 3, [DTO])
    assert not [m for m in after if m.arbitration_id == DTO and m.data[0] < 0xFC and m.timestamp > stopped], "DAQ still running after stop"

    samples = [m for m in frames if m.arbitration_id == DTO and m.data[0] == first_pid]
    temps = [m for m in frames if m.arbitration_id == DTO and m.data[0] == first_pid + 1]
    status = [m for m in frames if m.arbitration_id == STATUS.frame_id]
    # One sample per cycle, each the value the same cycle put on the bus.
    assert len(samples) == pytest.approx(len(status), abs=1), (len(samples), len(status))
    assert len(temps) == pytest.approx(len(samples), abs=1), (len(samples), len(temps))
    assert_period([b.timestamp - a.timestamp for a, b in zip(samples, samples[1:])], PERIOD_S, 0.05)
    ic = [round(struct.unpack("<f", m.data[1:5])[0], 2) for m in samples]
    assert set(ic) <= {input_current(m) for m in status}

    # The bench frame is untouched: every cycle present, counter unbroken.
    counters = [STATUS.decode(m.data)["Counter"] for m in status]
    assert all((b - a) % 128 == 1 for a, b in zip(counters, counters[1:])), counters


def test_daq_info_matches_a2l(xcp, a2l_path, a2l_text, check):
    # What the node reports of its DAQ processor is what the A2L tells a tool.
    a2l = pytest.importorskip("zelos_can.a2l", reason="this zelos-can has no A2L reader").load(str(a2l_path), strict=True)
    check.that(len(a2l["warnings"]), "==", 0, name="A2L loads strictly, without warnings")
    check.that(a2l["epk"]["string"], "==", xcp.identifier(5), name="EPK")

    props = xcp.getDaqProcessorInfo().daqProperties
    check.that(props.timestampSupported and props.prescalerSupported, name="timestamps and prescaler supported")
    res = xcp.getDaqResolutionInfo()
    mode = res.timestampMode
    stamp = a2l["daq"]["timestamp"]
    check.that(f"{mode.size} {mode.unit} {res.timestampTicks}", "==", "S4 DAQ_TIMESTAMP_UNIT_1US 1", name="timestamp: 4 bytes, 1 us")
    check.that(f"{stamp['size']} {stamp['unit']} {stamp['ticks']}", "==", "SIZE_DWORD UNIT_1US 1", name="A2L TIMESTAMP_SUPPORTED")
    check.that(mode.fixed or stamp["fixed"], "is_false", name="timestamps per list, not fixed")

    info = xcp.getDaqEventInfo(0)
    event = a2l["events"][0]
    check.that(f"{info.eventChannelTimeCycle} {int(info.eventChannelTimeUnit)}", "==", f"{event['cycle']} {event['cycle_unit']}", name="event 0 cycle")
    check.that(all(m["events"]["default"] == [0] for m in a2l["measurements"]), name="every measurement defaults to event 0")
    # All lists on the event are sampled in one go, never torn between cycles.
    check.that(str(info.daqEventProperties.consistency), "==", "CONSISTENCY_EVENTCHANNEL", name="event consistency")
    check.that(bool(re.search(r"\bCONSISTENCY\s+EVENT\b", a2l_text)), name="A2L CONSISTENCY EVENT")
    # A dropped sample is reported, not silent.
    check.that(props.overloadEvent and not props.overloadMsb, name="overload reported by event")
    check.that(bool(re.search(r"\bOVERLOAD_INDICATION_EVENT\b", a2l_text)), name="A2L OVERLOAD_INDICATION_EVENT")


def test_daq_timestamps(bus, bms, recorded, xcp, a2l, check):
    first_pid = daq_list(xcp, a2l, ["input_current_a"], DAQ_TIMESTAMP)
    mode = xcp.getDaqListMode(0)
    check.that(f"{mode.currentMode.selected} {mode.currentMode.timestamp} {mode.currentMode.running}", "==", "True True False", name="selected, timestamped, stopped")
    check.that(f"{mode.currentEventChannel} {mode.currentPrescaler}", "==", "0 1", name="event 0, prescaler 1")

    before = xcp.getDaqClock().timestamp
    xcp.startStopSynch(1)
    check.that(xcp.getDaqListMode(0).currentMode.running, name="running once started")
    frames = cycles(bus, 40, [DTO])
    xcp.startStopSynch(0)
    after = xcp.getDaqClock().timestamp
    check.that(xcp.getDaqListMode(0).currentMode.running, "is_false", name="stopped")

    # The node's view, into the trace: its timestamp and the float of the same sample.
    samples = odt(frames, DTO, first_pid)
    node = zelos_sdk.TraceSource("dcdc_xcp")
    for stamp, value in zip(samples, odt(frames, DTO, first_pid + 1)):
        node.log_at(int(value.timestamp * 1e9), "cycle_50ms", {"timestamp_us": timestamp(stamp), "input_current_a": struct.unpack("<f", value.data[1:5])[0]})

    # Stamped by the clock GET_DAQ_CLOCK reads, in order, between the two reads.
    since = [ticks(before, timestamp(m)) for m in samples]
    check.that(since == sorted(since) and 0 < since[0] and since[-1] < ticks(before, after), name="timestamps ordered, between two GET_DAQ_CLOCK reads")
    # One cycle apart in the node's time.
    gaps = [ticks(timestamp(a), timestamp(b)) * TIMESTAMP_TICK_S for a, b in zip(samples, samples[1:])]
    check.that(sum(gaps) / len(gaps), "is_close", PERIOD_S, rel_tol=0.02, name="mean timestamp step, s")
    check.that(max(gaps), "<", 2 * PERIOD_S, name="no sample missed")
    # The first ODT holds the timestamp alone; the float follows in the second.
    check.that(all(len(m.data) == 1 + 4 for m in samples), name="ODT 0: PID and timestamp")
    check.that(abs(len(odt(frames, DTO, first_pid + 1)) - len(samples)), "<=", 1, name="ODT 1 per sample")
    # Nominal load drops nothing.
    check.that(len([m for m in frames if bytes(m.data[:2]) == EV_DAQ_OVERLOAD]), "==", 0, name="no DAQ overload")


@pytest.mark.hardware
def test_daq_timestamps_keep_host_time(bus, bms, recorded, xcp, a2l, check):
    # The node's 1 us ticks against the host's receive stamps, over 5 s.
    first_pid = daq_list(xcp, a2l, ["input_current_a"], DAQ_TIMESTAMP)
    xcp.startStopSynch(1)
    samples = odt(cycles(bus, 100, [DTO]), DTO, first_pid)
    xcp.startStopSynch(0)
    node_s = ticks(timestamp(samples[0]), timestamp(samples[-1])) * TIMESTAMP_TICK_S
    host_s = samples[-1].timestamp - samples[0].timestamp
    check.that(node_s / host_s, "is_close", 1.0, abs_tol=0.02, name="node clock rate / host clock rate")


def test_daq_frozen_while_running(bus, bms, recorded, xcp, a2l, check):
    first_pid = daq_list(xcp, a2l, ["input_current_a"])
    xcp.startStopSynch(1)
    # A running list's layout cannot change under it.
    xcp.setDaqPtr(0, 0, 0)
    for what, call in (
        ("ALLOC_DAQ", lambda: xcp.allocDaq(1)),
        ("ALLOC_ODT", lambda: xcp.allocOdt(0, 1)),
        ("ALLOC_ODT_ENTRY", lambda: xcp.allocOdtEntry(0, 0, 1)),
        ("WRITE_DAQ", lambda: xcp.writeDaq(0xFF, 4, 0, a2l["temperature_c"])),
        ("SET_DAQ_LIST_MODE", lambda: xcp.setDaqListMode(DAQ_TIMESTAMP, 0, 0, 1, 0)),
    ):
        check.that(error(call), "==", ERR_DAQ_ACTIVE, name=f"{what} while running: ERR_DAQ_ACTIVE")
    # And it keeps sampling as it was set up: input current, no timestamp.
    drain(bus)
    frames = cycles(bus, 5, [DTO, STATUS.frame_id])
    samples = odt(frames, DTO, first_pid)
    check.that(len(samples), ">=", 4, name="samples over 5 cycles")
    check.that(all(len(m.data) == 1 + 4 for m in samples), name="layout unchanged: one float, no timestamp")
    sampled = {round(struct.unpack("<f", m.data[1:5])[0], 2) for m in samples}
    check.that(sampled <= {input_current(m) for m in frames if m.arbitration_id == STATUS.frame_id}, name="samples are the bus's InputCurrent")


def test_mode_not_valid(xcp, a2l, check):
    daq_list(xcp, a2l, ["input_current_a"])
    check.that(error(lambda: xcp.startStopDaqList(3, 0)), "==", ERR_MODE_NOT_VALID, name="START_STOP_DAQ_LIST mode 3")
    check.that(error(lambda: xcp.startStopSynch(3)), "==", ERR_MODE_NOT_VALID, name="START_STOP_SYNCH mode 3")
    # DAQ only: no STIM.
    check.that(error(lambda: xcp.setDaqListMode(DAQ_STIM, 0, 0, 1, 0)), "==", ERR_MODE_NOT_VALID, name="SET_DAQ_LIST_MODE STIM")


def test_second_master(bus, bms, recorded, xcp, a2l, check):
    first_pid = daq_list(xcp, a2l, ["input_current_a"])
    xcp.startStopSynch(1)
    check.that(len(odt(cycles(bus, 3, [DTO]), DTO, first_pid)), ">", 0, name="sampling")

    # Another master's CONNECT starts a clean session: our list stops. No
    # sample follows the node's response to it, over three cycles.
    drain(bus)
    bus.send(can.Message(arbitration_id=CRO, data=[0xFF, 0x00], is_extended_id=False))
    frames = until(bus, lambda frames: (i := connected(frames)) is not None and count(frames[i:], STATUS.frame_id) >= 3, ids=[DTO, STATUS.frame_id])
    check.that(len(odt(frames[connected(frames) :], DTO, first_pid)), "==", 0, name="samples after another master's CONNECT")

    # Recovery: the response to that CONNECT, which this master did not ask
    # for, is stale. Drop it, see the lists stopped, connect, set up again.
    with xcp.transport.resQueue_condition:
        xcp.transport.resQueue.clear()
    check.that(xcp.getStatus().sessionStatus.daqRunning, "is_false", name="GET_STATUS: DAQ stopped")
    xcp.connect()
    first_pid = daq_list(xcp, a2l, ["input_current_a"])
    xcp.startStopSynch(1)
    until(bus, lambda frames: len(odt(frames, DTO, first_pid)) >= 3, ids=[DTO])
