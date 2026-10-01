"""The PDU node as a CANopen slave, driven by python canopen as the master.

The master reads nodes/pdu/od/pdu.eds, the file the node's object dictionary
is generated from. Each test resets the node over NMT and waits for its
boot-up, so none depends on a fresh boot or on an earlier test.
"""

import struct
import time
from pathlib import Path

import canopen
import pytest

from zelos_testing import frames as bench
from zelos_testing.frames import WAIT_S, bound

# testing/suites/canopen/ -> the repository root. The bench's tester mounts
# nodes/ at the same place relative to its copy of this suite.
EDS = Path(__file__).resolve().parents[3] / "nodes" / "pdu" / "od" / "pdu.eds"

NODE_ID = 0x20
HEARTBEAT = 0x700 + NODE_ID
EMCY = 0x080 + NODE_ID
TPDO1 = 0x180 + NODE_ID
TPDO2 = 0x280 + NODE_ID
RPDO1 = 0x200 + NODE_ID

BOOTUP, STOPPED, OPERATIONAL, PRE_OPERATIONAL = 0x00, 0x04, 0x05, 0x7F
HEARTBEAT_S = 1.0
# TPDO1's event timer: it repeats this often even when nothing changes.
TPDO1_EVENT_S = 0.5

# From nodes/pdu: each channel's load when powered, 0.1 A. Channel 8's is
# shorted: enabling it trips the channel and raises EMCY 0x2300 (current,
# output side) with error bit 0x30 + channel index and the channel as info.
LOAD_DA = [20, 50, 15, 30, 5, 80, 40, 220]
SHORTED = 8
EMC_CURRENT_OUTPUT = 0x2300
INPUTS = "Read input 8-bit.Input 1 to 8"
OUTPUTS = "Write output 8-bit.Output 1 to 8"

pytestmark = pytest.mark.usefixtures("bus_health")


class Frames:
    """Frames the node sent on some COB-IDs, as (timestamp, data)."""

    def __init__(self, network, cob_ids):
        self.by_id = {cob: [] for cob in cob_ids}
        for cob in cob_ids:
            network.subscribe(cob, self._on)

    def _on(self, cob, data, timestamp):
        self.by_id[cob].append((timestamp, bytes(data)))

    def __getitem__(self, cob):
        return self.by_id[cob]


def wait_for(done, what):
    """done()'s first truthy result. The Notifier owns the bus, so this polls what it caught."""
    end = time.monotonic() + bound(WAIT_S)
    while time.monotonic() < end:
        if result := done():
            return result
        time.sleep(0.02)
    pytest.fail(f"timed out waiting for {what}")


def bootup(node, t0):
    """When the node announced its first boot-up after t0."""
    return wait_for(lambda: next((ts for ts, d in node.frames[HEARTBEAT] if ts > t0 and d == bytes([BOOTUP])), None), "boot-up")


@pytest.fixture
def node(bus):
    network = canopen.Network(bus)
    pdu = network.add_node(NODE_ID, str(EDS))
    pdu.frames = Frames(network, (HEARTBEAT, EMCY, TPDO1, TPDO2))
    # An SDO answer takes the node no time, but Renode's clock runs slow.
    pdu.sdo.RESPONSE_TIMEOUT = bound(pdu.sdo.RESPONSE_TIMEOUT)
    network.connect()
    try:
        reset_at = time.time()
        pdu.nmt.state = "RESET"
        pdu.booted_at = bootup(pdu, reset_at)
        yield pdu
    finally:
        # The bus belongs to its fixture; only stop listening on it.
        network.notifier.stop()


def after(node, cob, t0, count):
    """The first `count` frames on cob sent after t0."""

    def ready():
        got = [f for f in node.frames[cob] if f[0] > t0]
        return got[:count] if len(got) >= count else None

    return wait_for(ready, f"{count} frames on {cob:#05x}")


def states_after(node, t0, count):
    """NMT states from the first `count` heartbeats after t0."""
    return [data[0] & 0x7F for _, data in after(node, HEARTBEAT, t0, count)]


def reaches(node, t0, code):
    """Wait for a heartbeat after t0 in this NMT state."""
    wait_for(lambda: code in [d[0] & 0x7F for ts, d in node.frames[HEARTBEAT] if ts > t0], f"heartbeat state {code:#04x}")


def enter(node, state, code):
    t0 = time.time()
    node.nmt.state = state
    # The new state shows in the next periodic heartbeat, not at once.
    reaches(node, t0, code)


def test_boots_pre_operational_with_heartbeat(node):
    # A heartbeat already on its way when the reset went out can arrive after
    # it, so the count starts at the boot-up.
    beats = after(node, HEARTBEAT, node.booted_at, 3)
    assert [data for _, data in beats] == [bytes([PRE_OPERATIONAL])] * 3

    gaps = [b[0] - a[0] for a, b in zip(beats, beats[1:])]
    if bench.SIMULATED:
        # Virtual time only ever runs slow against the host's clock.
        assert all(g > 0.95 * HEARTBEAT_S for g in gaps), gaps
    else:
        assert all(abs(g - HEARTBEAT_S) < 0.05 * HEARTBEAT_S for g in gaps), gaps


def test_nmt_start_stop(node):
    t0 = time.time()
    enter(node, "OPERATIONAL", OPERATIONAL)
    wait_for(lambda: [f for f in node.frames[TPDO1] if f[0] > t0], "TPDO1 once operational")

    enter(node, "STOPPED", STOPPED)
    t0 = time.time()
    # Stopped: heartbeats only, no PDOs, for longer than any PDO's event timer.
    assert states_after(node, t0, 2) == [STOPPED, STOPPED]
    assert not [f for cob in (TPDO1, TPDO2) for f in node.frames[cob] if f[0] > t0]

    enter(node, "PRE-OPERATIONAL", PRE_OPERATIONAL)


def test_sdo_identity(node):
    od = node.object_dictionary
    assert node.sdo[0x1000].raw == od[0x1000].default == 0x00030191  # CiA 401, DI + DO
    for sub in range(1, 5):
        assert node.sdo[0x1018][sub].raw == od[0x1018][sub].default
    assert node.sdo[0x1008].raw == "Zelos PDU"
    assert node.sdo[0x1017].raw == HEARTBEAT_S * 1000


def expect_io(outputs):
    """Inputs and currents of a healthy PDU with these outputs commanded."""
    on = [bool(outputs & 1 << ch) and ch + 1 != SHORTED for ch in range(8)]
    inputs = sum(1 << ch for ch in range(8) if on[ch])
    return inputs, [LOAD_DA[ch] if on[ch] else 0 for ch in range(8)]


def sdo_io(node):
    return node.sdo[0x6000][1].raw, [node.sdo[0x2000][ch].raw for ch in range(1, 9)]


def test_sdo_download_to_outputs(node):
    for outputs in (0b0000_0101, 0b0110_0000, 0):
        node.sdo[0x6200][1].raw = outputs
        assert node.sdo[0x6200][1].raw == outputs
        wait_for(lambda: sdo_io(node) == expect_io(outputs), f"inputs for outputs {outputs:#04x}")


def tpdo_io(node):
    return node.tpdo[1][INPUTS].raw, [var.raw for var in node.tpdo[2]]


def command(node, outputs):
    node.rpdo[1][OUTPUTS].raw = outputs
    node.rpdo[1].transmit()


def test_rpdo_command_reflected_by_tpdos(node):
    node.tpdo.read()
    node.rpdo.read()
    assert (node.rpdo[1].cob_id, node.tpdo[1].cob_id, node.tpdo[2].cob_id) == (RPDO1, TPDO1, TPDO2)

    enter(node, "OPERATIONAL", OPERATIONAL)
    for outputs in (0b0000_0011, 0b0100_1000):
        command(node, outputs)
        wait_for(lambda: tpdo_io(node) == expect_io(outputs), f"TPDOs for outputs {outputs:#04x}")

    # Nothing changes now, so TPDO1 repeats on its event timer alone.
    t0 = time.time()
    wait_for(lambda: len([f for f in node.frames[TPDO1] if f[0] > t0]) >= 3, "TPDO1 event timer")
    stamps = [f[0] for f in node.frames[TPDO1] if f[0] > t0]
    assert all(b - a > 0.9 * TPDO1_EVENT_S for a, b in zip(stamps, stamps[1:])), stamps


def emcys_after(node, t0):
    """EMCY frames after t0 as (code, error register, error bit, info)."""
    return [struct.unpack("<HBBI", data) for ts, data in node.frames[EMCY] if ts > t0]


def test_overcurrent_trips_with_emcy_and_recovers(node):
    node.tpdo.read()
    node.rpdo.read()
    shorted = 1 << (SHORTED - 1)

    # Twice, to show the recovery is complete.
    for _ in range(2):
        enter(node, "OPERATIONAL", OPERATIONAL)
        t0 = time.time()
        command(node, 0b0000_0001 | shorted)
        code, register, error_bit, info = wait_for(lambda: emcys_after(node, t0), "EMCY")[0]
        assert (code, error_bit, info) == (EMC_CURRENT_OUTPUT, 0x30 + SHORTED - 1, SHORTED)
        assert register != 0

        # 0x1029 is the CiA default: the error takes the node to
        # pre-operational, where PDOs stop and SDO still works.
        reaches(node, t0, PRE_OPERATIONAL)
        assert sdo_io(node) == expect_io(0b0000_0001)

        # Commanding the channel off clears the trip: EMCY error reset.
        t0 = time.time()
        node.sdo[0x6200][1].raw = 0b0000_0001
        code, register, _, _ = wait_for(lambda: emcys_after(node, t0), "EMCY reset")[0]
        assert (code, register) == (0x0000, 0)

    # A communication reset clears the node's error state but not the short:
    # the channel trips again, and says so again.
    enter(node, "OPERATIONAL", OPERATIONAL)
    t0 = time.time()
    command(node, shorted)
    wait_for(lambda: emcys_after(node, t0), "EMCY")
    t0 = time.time()
    node.nmt.state = "RESET COMMUNICATION"
    trips = wait_for(lambda: [e for e in emcys_after(node, t0) if e[0] == EMC_CURRENT_OUTPUT], "EMCY after the reset")
    _, register, error_bit, info = trips[0]
    assert (error_bit, info) == (0x30 + SHORTED - 1, SHORTED)
    assert register != 0
    node.sdo[0x6200][1].raw = 0
