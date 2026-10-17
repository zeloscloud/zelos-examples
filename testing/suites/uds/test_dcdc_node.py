"""The DC-DC node itself over UDS: what a UDS server alone does not show.

Its DCDC_Status broadcast and model behind the DIDs, DTCs over operation cycles
with the suite playing the BMS, a reset as a fresh boot, and XCP alongside. Skips
where no DC-DC is on the bus. The server contract is test_uds_conformance.py.
"""

import struct
import time

import pytest
from pyxcp.types import XcpTimeoutError
from udsoncan.services import ECUReset

from zelos_testing import frames
from zelos_testing.bench_plugin import find_a2l
from zelos_testing.dcdc import DB, LIMITS, LIMITS_PERIOD_S, STATUS, BmsLimits
from zelos_testing.frames import Periodic, bound, collect, count, until
from zelos_testing.uds import (
    ACTIVE_SESSION,
    ADJUST,
    CALIBRATION_ADDRESS,
    CDTC,
    CURRENT,
    DEFAULT,
    DTC_BMS_LOST,
    DTC_OVERCURRENT,
    DTC_OVERTEMPERATURE,
    DTC_UNDERVOLTAGE,
    EXTENDED,
    INPUT_V,
    LEVEL_EXTENDED,
    MARGIN,
    OUTPUT,
    PDTC,
    S3_S,
    SELF_TEST,
    SETPOINT,
    SETPOINT_V,
    SNAPSHOT_DIDS,
    TEMPERATURE,
    TF,
    TFSLC,
    TFTOC,
    VOLTAGE,
    WIR,
    dtcs,
    location,
    reset,
)
from zelos_testing.xcp import Ecu as XcpEcu

BMS_STATUS = DB.get_message_by_name("BMS_Status")

pytestmark = pytest.mark.usefixtures("bus_health", "dcdc_node")


def wait_until(done, node_s: float, step_s: float = 0.1):
    """Poll done() until it holds, within node_s of the server's time."""
    end = time.monotonic() + bound(node_s)
    while not done():
        assert time.monotonic() < end, f"not done within {node_s} s"
        time.sleep(step_s)



def cycles(bus, n: int):
    """Wait out the node's next n DCDC_Status cycles, not ones already received."""
    after = time.time()
    until(bus, lambda f: sum(m.timestamp > after for m in f) >= n, ids=[STATUS.frame_id])



@pytest.fixture(scope="session")
def dcdc_node(pytestconfig, uds_server):
    """The DC-DC node itself is on the bus: its DCDC_Status frames are."""
    import can

    with can.Bus(interface=pytestconfig.getoption("interface"), channel=pytestconfig.getoption("channel")) as b:
        if not collect(b, bound(0.5), ids=[STATUS.frame_id]):
            pytest.skip("no DCDC_Status on the bus: not the DC-DC node")



@pytest.fixture
def no_bms(bus):
    """No BMS on the bus: the suite plays it, and stops it. Skips if a real one is there."""
    seen = until(bus, lambda f: f[-1].arbitration_id == LIMITS.frame_id or count(f, STATUS.frame_id) >= 20)
    if seen[-1].arbitration_id == LIMITS.frame_id:
        pytest.skip("a BMS is on the bus: the suite cannot set or stop what it sends")


@pytest.fixture
def bms(bus, no_bms):
    """BMS_Limits granting 20 A, played by the suite."""
    with Periodic(bus, BmsLimits(aux=20.0), LIMITS_PERIOD_S):
        yield



def test_dtcs_follow_the_model_over_operation_cycles(ecu, bus, no_bms):
    # BMS_Limits missing: the node's own test fails, with a snapshot.
    with Periodic(bus, BmsLimits(aux=20.0), LIMITS_PERIOD_S):
        cycles(bus, 4)
        ecu.client.clear_dtc(0xFFFFFF)
        cycles(bus, 2)
        assert dtcs(ecu, TF | CDTC).get(DTC_BMS_LOST) is None
    # The first failed cycle: pending, not yet confirmed (OBD-II two-cycle confirmation).
    wait_until(lambda: dtcs(ecu, TF).get(DTC_BMS_LOST) == TF | TFTOC | PDTC | TFSLC, 2.0)
    # Snapshot records: OBD-II's first frame, the node's DIDs, OBD-II's second frame.
    assert ecu.raw(b"\x19\x03") == b"\x59\x03" + b"".join(b"\xc1\x11\x00" + bytes([r]) for r in (0x00, 0x01, 0xF0))
    snapshot = ecu.client.get_dtc_snapshot_by_dtc_number(DTC_BMS_LOST, 1).service_data.dtcs[0].snapshots[0]
    assert snapshot.record_number == 1 and snapshot.did in SNAPSHOT_DIDS
    assert ecu.raw(b"\x19\x06\xc1\x11\x00\x01")[6:] == b"\x01\x01"
    # A reset is a new operation cycle; the memory survives it. Failed again: confirmed, MIL on.
    reset(ecu)
    wait_until(lambda: dtcs(ecu, TF).get(DTC_BMS_LOST) == TF | TFTOC | PDTC | CDTC | TFSLC | WIR, 2.0)
    assert ecu.raw(b"\x19\x06\xc1\x11\x00\x01")[6:] == b"\x01\x02"

    with Periodic(bus, BmsLimits(aux=20.0), LIMITS_PERIOD_S):
        # Healed this cycle: confirmed, pending and the MIL stay.
        wait_until(lambda: dtcs(ecu).get(DTC_BMS_LOST) == TFTOC | PDTC | CDTC | TFSLC | WIR, 2.0)
        reset(ecu)
        cycles(bus, 4)
        assert dtcs(ecu).get(DTC_BMS_LOST) == PDTC | CDTC | TFSLC | WIR
        # Passing a whole cycle clears pending; three turn the MIL off; confirmed stays (40 to age).
        for status in (CDTC | TFSLC | WIR, CDTC | TFSLC | WIR, CDTC | TFSLC):
            reset(ecu)
            cycles(bus, 4)
            assert dtcs(ecu).get(DTC_BMS_LOST) == status
    assert ecu.raw(b"\x19\x06\xc1\x11\x00\x01")[6:] == b"\x01\x02"  # two cycles failed


def test_dtc_setting_off_freezes_status(ecu, bus, no_bms):
    with Periodic(bus, BmsLimits(aux=20.0), LIMITS_PERIOD_S):
        cycles(bus, 4)
        ecu.client.clear_dtc(0xFFFFFF)
        ecu.enter(EXTENDED)
        ecu.unlock(LEVEL_EXTENDED)
        ecu.client.control_dtc_setting(2)
    cycles(bus, 20)  # a second without BMS_Limits
    assert dtcs(ecu, TF | CDTC).get(DTC_BMS_LOST) is None
    ecu.client.control_dtc_setting(1)
    wait_until(lambda: dtcs(ecu, TF).get(DTC_BMS_LOST) is not None, 2.0)


def test_undervoltage_overcurrent_and_overtemperature(ecu, bus, bms):
    pack = {"PackVoltage": 200.0, "PackCurrent": 0.0, "StateOfCharge": 50.0, "ContactorState": 1, "Counter": 0}
    with Periodic(bus, lambda: _frame(BMS_STATUS, pack), 0.1):
        wait_until(lambda: dtcs(ecu, TF).get(DTC_UNDERVOLTAGE) is not None, 2.0)
        assert ecu.read(INPUT_V) == 200.0
        pack["PackVoltage"] = 380.0
        wait_until(lambda: DTC_UNDERVOLTAGE not in dtcs(ecu, TF), 2.0)
    # Demand recalibrated to 20 A by address: over 300 A out, then over 70 degC.
    ecu.enter(EXTENDED)
    ecu.unlock(LEVEL_EXTENDED)
    original = ecu.client.read_memory_by_address(location(CALIBRATION_ADDRESS, 4)).service_data.memory_block
    ecu.client.write_memory_by_address(location(CALIBRATION_ADDRESS, 4), struct.pack("<f", 20.0))
    try:
        wait_until(lambda: dtcs(ecu, TF).get(DTC_OVERCURRENT) is not None, 4.0)
        wait_until(lambda: dtcs(ecu, TF).get(DTC_OVERTEMPERATURE) is not None, 15.0)
        assert ecu.client.start_routine(SELF_TEST).service_data.routine_status_record == bytes([0b0101])
    finally:
        ecu.client.write_memory_by_address(location(CALIBRATION_ADDRESS, 4), original)


def _frame(message, signals):
    import can

    return can.Message(arbitration_id=message.frame_id, data=message.encode(signals), is_extended_id=False)



def status(bus) -> dict:
    """The next DCDC_Status, decoded."""
    after = time.time()
    return STATUS.decode(until(bus, lambda f: f[-1].timestamp > after, ids=[STATUS.frame_id])[-1].data)


def test_live_values_match_the_broadcast(ecu, bus):
    # The model keeps running: bracket the read by the DCDC_Status frames just
    # before and after it. Each value lies within what they span, to the
    # frame's resolution, however slowly the read went.
    started = time.time()
    first = until(bus, lambda f: f[-1].timestamp > started, ids=[STATUS.frame_id])[-1]
    values = ecu.client.read_data_by_identifier([VOLTAGE, CURRENT, TEMPERATURE]).service_data.values
    read_at = time.time()
    rest = until(bus, lambda f: f[-1].timestamp > read_at, ids=[STATUS.frame_id])
    seen = [STATUS.decode(m.data) for m in (first, *rest)]
    for did, signal, tol in ((VOLTAGE, "OutputVoltage", 0.05), (CURRENT, "OutputCurrent", 1.0), (TEMPERATURE, "Temperature", 1.5)):
        lo, hi = min(f[signal] for f in seen), max(f[signal] for f in seen)
        assert lo - tol <= values[did] <= hi + tol, f"{signal}: {values[did]} outside {lo}..{hi} +-{tol}"


def test_setpoint_drives_the_output(ecu, bus):
    ecu.enter(EXTENDED)
    ecu.unlock(LEVEL_EXTENDED)
    ecu.client.write_data_by_identifier(SETPOINT, 12.0)
    try:
        # Less the droop under load.
        wait_until(lambda: 11.0 < status(bus)["OutputVoltage"] <= 12.0, 1.0)
        ecu.client.io_control(OUTPUT, control_param=ADJUST, values=[15.0])
        wait_until(lambda: 14.0 < status(bus)["OutputVoltage"] <= 15.0, 1.0)
    finally:
        ecu.client.write_data_by_identifier(SETPOINT, SETPOINT_V)
    # Leaving the session returns the output to the ECU, at the setpoint.
    ecu.client.change_session(DEFAULT)
    wait_until(lambda: 13.0 < status(bus)["OutputVoltage"] <= SETPOINT_V, 1.0)


def test_communication_control_stops_the_broadcast(ecu, bus):
    confirmed = set(dtcs(ecu, CDTC))
    ecu.enter(EXTENDED)
    ecu.unlock(LEVEL_EXTENDED)
    ecu.client.communication_control(0x01, 0x01)
    sent_at = time.time()
    assert ecu.read(ACTIVE_SESSION) == EXTENDED  # diagnostics still answer
    # One frame may already be on its way: allow a cycle.
    quiet = [m for m in collect(bus, bound(0.5), ids=[STATUS.frame_id]) if m.timestamp > sent_at + bound(STATUS.cycle_time / 1000)]
    assert quiet == [], "DCDC_Status sent with transmission disabled"
    ecu.client.communication_control(0x00, 0x01)
    status(bus)
    # Left alone, S3 ends the session and restores it.
    ecu.client.communication_control(0x03, 0x01)
    stopped_at = time.time()
    resumed = until(bus, lambda f: f[-1].timestamp > stopped_at + 0.1, node_s=2 * S3_S, ids=[STATUS.frame_id])[-1].timestamp
    if not frames.SIMULATED:
        assert resumed - stopped_at > S3_S * (1 - MARGIN)
    assert ecu.session() == DEFAULT
    # Reception's gap is not a lost BMS: its timeout restarts with reception.
    cycles(bus, 2)
    assert set(dtcs(ecu, CDTC)) <= confirmed


def counter(msg) -> int:
    return STATUS.decode(msg.data)["Counter"]


def test_reset_is_a_fresh_boot(ecu, bus):
    # DCDC_Status's counter starts again from 0: a break in its sequence.
    last = until(bus, lambda f: True, ids=[STATUS.frame_id])[-1:]
    ecu.client.ecu_reset(ECUReset.ResetType.hardReset)
    seen = until(bus, lambda f: counter(f[-1]) != (counter(([*last, *f])[-2]) + 1) % 128, ids=[STATUS.frame_id])
    assert counter(seen[-1]) == 0


def test_xcp_and_uds_share_the_ecu(pytestconfig, ecu, bus):
    a2l = find_a2l(pytestconfig)
    if a2l is None:
        pytest.skip("no A2L: needs the `-S xcp -S uds` build and --a2l")
    channel = pytestconfig.getoption("channel")
    with XcpEcu(channel, a2l, timeout_s=bound(2.0)) as xcp:
        # One calibration, two tools.
        ecu.enter(EXTENDED)
        ecu.unlock(LEVEL_EXTENDED)
        original = xcp.read("DEMAND_A")
        ecu.client.write_memory_by_address(location(CALIBRATION_ADDRESS, 4), struct.pack("<f", 1.5))
        assert xcp.read("DEMAND_A") == pytest.approx(1.5)
        # A UDS reset ends the XCP session: the master must connect again.
        reset(ecu)
        with pytest.raises(XcpTimeoutError):
            xcp.read("temperature_c")
    with XcpEcu(channel, a2l, timeout_s=bound(2.0)) as xcp:
        assert xcp.epk() == xcp.a2l["epk"]["string"]
        # The calibration was RAM: the reset restored the build's.
        assert xcp.read("DEMAND_A") == pytest.approx(original)
