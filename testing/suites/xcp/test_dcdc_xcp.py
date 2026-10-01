"""The DC-DC node's XCP slave, driven by pyxcp as an independent master.

Needs the `-S xcp` build and the A2L generated beside its ELF (--a2l). The suite
plays the BMS unless a real one is on the bus. Every test connects afresh and
sets what it relies on, and a calibration test restores the value it found.
"""

import logging
import os
import re
import struct
import time

import can
import pytest

from zelos_testing import dbc
from zelos_testing.frames import Periodic, collect

# pyxcp stamps its session with the local zone and fails on abbreviations
# such as PST, so it runs in UTC.
os.environ["TZ"] = "UTC"
time.tzset()

from pyxcp.config import create_application_from_config, set_application  # noqa: E402
from pyxcp.master import Master  # noqa: E402
from pyxcp.types import XcpResponseError  # noqa: E402

CRO, DTO = 0x6F0, 0x6F1
ERR_ACCESS_DENIED = 0x24

DB = dbc.load("bench")
STATUS = DB.get_message_by_name("DCDC_Status")
LIMITS = DB.get_message_by_name("BMS_Limits")
PERIOD_S = STATUS.cycle_time / 1000
# The allowance this suite sends when it plays the BMS.
AUX_A = 10.0
# A demand below every allowance either BMS grants (a real one: 2 to 6 A by
# mode), so the current it settles at does not depend on which BMS is present.
LOW_A = 1.5
TIMEOUT_S = 20.0

pytestmark = pytest.mark.usefixtures("bus_health")


@pytest.fixture(scope="module")
def a2l(pytestconfig):
    """Object name -> address, from the generated A2L."""
    path = pytestconfig.getoption("a2l")
    if not path:
        pytest.fail("pass --a2l with the A2L built beside the ELF")
    text = open(path).read()
    addrs = {}
    for kind, name, body in re.findall(r"/begin (MEASUREMENT|CHARACTERISTIC) (\S+)(.*?)/end \1", text, re.S):
        field = r"ECU_ADDRESS\s+(0x[0-9A-Fa-f]+)" if kind == "MEASUREMENT" else r'^\s*"[^"]*"\s+\S+\s+(0x[0-9A-Fa-f]+)'
        addrs[name] = int(re.search(field, body).group(1), 16)
    assert all(addrs.values()), f"placeholder addresses in {path}: not the generated A2L?"
    return addrs


@pytest.fixture
def xcp(pytestconfig):
    app = create_application_from_config(
        {
            "Transport": {
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
    if collect(bus, 1.0, [LIMITS.frame_id]):
        yield
        return
    counter = 0

    def msg():
        nonlocal counter
        counter = (counter + 1) & 0xFF
        data = LIMITS.encode(
            {
                "DischargeCurrentLimit": 200.0,
                "ChargeCurrentLimit": 50.0,
                "AuxCurrentLimit": AUX_A,
                "Counter": counter,
            }
        )
        return can.Message(arbitration_id=LIMITS.frame_id, data=data, is_extended_id=False)

    with Periodic(bus, msg, LIMITS.cycle_time / 1000):
        yield


def read_f32(xcp, addr):
    return struct.unpack("<f", xcp.shortUpload(4, addr))[0]


def write_f32(xcp, addr, value):
    xcp.setMta(addr)
    xcp.download(struct.pack("<f", value))


class demand:
    """DEMAND_A calibrated to `amps` for the block, then restored."""

    def __init__(self, xcp, a2l, amps):
        self.xcp, self.addr, self.amps = xcp, a2l["DEMAND_A"], amps

    def __enter__(self):
        self.found = read_f32(self.xcp, self.addr)
        write_f32(self.xcp, self.addr, self.amps)
        return self

    def __exit__(self, *exc):
        write_f32(self.xcp, self.addr, self.found)
        assert read_f32(self.xcp, self.addr) == self.found


def input_current(bus, seconds):
    return [round(STATUS.decode(m.data)["InputCurrent"], 2) for m in collect(bus, seconds, [STATUS.frame_id])]


def next_allowance(bus):
    """AuxCurrentLimit of the next BMS_Limits on the bus."""
    end = time.monotonic() + TIMEOUT_S
    while time.monotonic() < end:
        if frames := collect(bus, 0.25, [LIMITS.frame_id]):
            return LIMITS.decode(frames[0].data)["AuxCurrentLimit"]
    pytest.fail("no BMS_Limits")


def settle(bus, amps):
    """Wait until DCDC_Status.InputCurrent reads `amps`."""
    seen = []
    end = time.monotonic() + TIMEOUT_S
    while time.monotonic() < end:
        seen += input_current(bus, 0.25)
        if seen and seen[-1] == amps:
            return
    pytest.fail(f"InputCurrent never reached {amps}: {seen[-20:]}")


def cycles(bus, count, ids):
    """Frames on ids, with DCDC_Status, over the node's next `count` cycles."""
    frames = []
    end = time.monotonic() + TIMEOUT_S
    while sum(m.arbitration_id == STATUS.frame_id for m in frames) < count:
        if time.monotonic() > end:
            pytest.fail(f"{count} cycles took over {TIMEOUT_S} s")
        frames += collect(bus, 0.25, [*ids, STATUS.frame_id])
    return frames


def test_connect(xcp):
    props = xcp.slaveProperties
    assert (props.maxCto, props.maxDto, str(props.byteOrder)) == (8, 8, "INTEL")
    assert xcp.identifier(1) == "dcdc"


def test_measurements(bus, bms, xcp, a2l):
    slew = read_f32(xcp, a2l["SLEW_A_PER_S"])
    with demand(xcp, a2l, LOW_A):
        settle(bus, LOW_A)
        # A real BMS changes the allowance with the VCU's mode, so the node
        # holds the allowance of a frame just before or just after the read.
        before = next_allowance(bus)
        aux = read_f32(xcp, a2l["aux_limit_a"])
        assert aux == pytest.approx(before, abs=0.01) or aux == pytest.approx(next_allowance(bus), abs=0.01)
        assert read_f32(xcp, a2l["setpoint_a"]) == pytest.approx(LOW_A)
        assert read_f32(xcp, a2l["input_current_a"]) == pytest.approx(LOW_A, abs=0.01)
    assert read_f32(xcp, a2l["slew_step_a"]) == pytest.approx(slew * PERIOD_S)
    on_bus = STATUS.decode(collect(bus, 0.2, [STATUS.frame_id])[-1].data)["Temperature"]
    assert read_f32(xcp, a2l["temperature_c"]) == pytest.approx(on_bus, abs=1.0)


def test_only_registered_memory(xcp, a2l):
    # A measurement is readable, not writable.
    xcp.setMta(a2l["input_current_a"])
    with pytest.raises(XcpResponseError) as e:
        xcp.download(b"\0\0\0\0")
    assert int(e.value.get_error_code()) == ERR_ACCESS_DENIED

    # Nothing outside the registered variables: not flash, not a read that
    # straddles a variable's end, not the word after the last one.
    for addr in (0x08000000, a2l["DEMAND_A"] + 2, max(a2l.values()) + 4):
        with pytest.raises(XcpResponseError) as e:
            xcp.shortUpload(4, addr)
        assert int(e.value.get_error_code()) == ERR_ACCESS_DENIED, hex(addr)


def test_calibration_is_live(bus, bms, xcp, a2l):
    # Down, then up, so the current is seen to follow the calibration both ways.
    with demand(xcp, a2l, LOW_A / 2):
        settle(bus, LOW_A / 2)
        write_f32(xcp, a2l["DEMAND_A"], LOW_A)
        assert read_f32(xcp, a2l["DEMAND_A"]) == LOW_A
        settle(bus, LOW_A)


def test_daq_on_cycle_event(bus, bms, xcp, a2l, pytestconfig):
    # One list on event 0 (the 50 ms cycle), two ODTs of one float each.
    xcp.freeDaq()
    xcp.allocDaq(1)
    xcp.allocOdt(0, 2)
    xcp.allocOdtEntry(0, 0, 1)
    xcp.allocOdtEntry(0, 1, 1)
    for odt, name in enumerate(("input_current_a", "temperature_c")):
        xcp.setDaqPtr(0, odt, 0)
        xcp.writeDaq(0xFF, 4, 0, a2l[name])
    xcp.setDaqListMode(0, 0, 0, 1, 0)
    first_pid = xcp.startStopDaqList(2, 0).firstPid
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
    if not pytestconfig.getoption("channel").startswith("vcan"):
        gaps = [b.timestamp - a.timestamp for a, b in zip(samples, samples[1:])]
        assert sum(gaps) / len(gaps) == pytest.approx(PERIOD_S, rel=0.05)
    ic = [round(struct.unpack("<f", m.data[1:5])[0], 2) for m in samples]
    assert set(ic) <= {round(STATUS.decode(m.data)["InputCurrent"], 2) for m in status}

    # The bench frame is untouched: every cycle present, counter unbroken.
    counters = [STATUS.decode(m.data)["Counter"] for m in status]
    assert all((b - a) % 128 == 1 for a, b in zip(counters, counters[1:])), counters
