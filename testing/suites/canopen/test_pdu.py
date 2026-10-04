"""The PDU node as a CANopen slave, with python canopen as the master.

What is tested, one visible behaviour per test: boot-up and heartbeat, NMT
state changes, SDO reads and writes, PDO commands and replies, an overcurrent
trip reported by EMCY, outputs on a communication error, and a PDO sent on
SYNC.

The master reads nodes/pdu/od/pdu.eds, the file the node's object dictionary is
generated from. Every test starts by resetting the node over NMT, so none
depends on a fresh boot or on an earlier test.

Run it against a bench (`cd can-full && just test`) or a board on an adapter:

    cd testing && uv run pytest suites/canopen --channel can0
"""

import time
from pathlib import Path

import canopen
import pytest

from zelos_testing.frames import WAIT_S, bound

# testing/suites/canopen/ -> the repository root.
EDS = Path(__file__).resolve().parents[3] / "nodes" / "pdu" / "od" / "pdu.eds"
NODE_ID = 0x20

# NMT states as the heartbeat reports them (CiA 301).
BOOTUP, STOPPED, OPERATIONAL, PRE_OPERATIONAL = 0x00, 0x04, 0x05, 0x7F

# From nodes/pdu: each channel's load when on, in 0.1 A. Channel 8 is shorted.
LOAD_DA = [20, 50, 15, 30, 5, 80, 40, 220]
SHORTED = 8
# EMCY error code 0x2300: current, device output side (CiA 301).
EMC_CURRENT_OUTPUT = 0x2300

# PDO entries by the names the EDS gives them.
INPUTS = "Read input 8-bit.Input 1 to 8"
OUTPUTS = "Write output 8-bit.Output 1 to 8"

pytestmark = pytest.mark.usefixtures("bus_health")


def wait_until(condition, what):
    """condition()'s first truthy result. python canopen records frames on its own thread."""
    end = time.monotonic() + bound(WAIT_S)
    while time.monotonic() < end:
        if result := condition():
            return result
        time.sleep(0.02)
    pytest.fail(f"timed out waiting for {what}")


def wait_for(read, want, what):
    """Poll read() until it returns want."""
    wait_until(lambda: read() == want, what)


def set_state(node, command, state):
    """Send an NMT command, then wait for the heartbeat to report the new state."""
    node.heartbeats.clear()
    node.nmt.state = command
    wait_until(lambda: state in node.heartbeats, f"heartbeat state {state:#04x}")


def expected(outputs):
    """Inputs and per-channel currents once these outputs are on. The shorted one stays off."""
    on = [bool(outputs & 1 << ch) and ch + 1 != SHORTED for ch in range(8)]
    return sum(1 << ch for ch in range(8) if on[ch]), [LOAD_DA[ch] if on[ch] else 0 for ch in range(8)]


@pytest.fixture
def node(bus):
    """The PDU, reset and pre-operational, with its PDOs mapped as the EDS declares."""
    network = canopen.Network(bus)
    node = network.add_node(NODE_ID, str(EDS))
    # Renode runs slower than real time, so SDO answers take longer there.
    node.sdo.RESPONSE_TIMEOUT = bound(node.sdo.RESPONSE_TIMEOUT)
    node.heartbeats = []
    node.nmt.add_heartbeat_callback(node.heartbeats.append)
    network.connect()
    try:
        # Reset node: the node restarts and announces it with a boot-up message.
        set_state(node, "RESET", BOOTUP)
        # A reset restores the default PDO mapping, which is the EDS's.
        node.tpdo.read(from_od=True)
        node.rpdo.read(from_od=True)
        yield node
    finally:
        # The bus belongs to its fixture; only stop listening on it.
        network.notifier.stop()


def test_boots_pre_operational_with_heartbeat(node):
    # After boot-up, the heartbeat reports pre-operational until told otherwise.
    wait_until(lambda: len(node.heartbeats) - node.heartbeats.index(BOOTUP) > 3, "three heartbeats after boot-up")
    after_bootup = node.heartbeats[node.heartbeats.index(BOOTUP) + 1 :]
    assert after_bootup[:3] == [PRE_OPERATIONAL] * 3


def test_nmt_start_stop(node):
    tpdo1 = node.tpdo[1]

    # Operational: PDOs flow. TPDO1 has an event timer, so it repeats on its own.
    set_state(node, "OPERATIONAL", OPERATIONAL)
    wait_until(lambda: tpdo1.timestamp, "TPDO1")

    # Stopped: only NMT and heartbeat. Two heartbeats outlast TPDO1's timer.
    set_state(node, "STOPPED", STOPPED)
    last = tpdo1.timestamp
    node.heartbeats.clear()
    wait_until(lambda: len(node.heartbeats) >= 2, "two heartbeats")
    assert node.heartbeats[-2:] == [STOPPED, STOPPED]
    assert tpdo1.timestamp == last

    set_state(node, "PRE-OPERATIONAL", PRE_OPERATIONAL)


def test_sdo_reads_identity(node):
    # 0x1000 device type: CiA 401, digital inputs and outputs.
    assert node.sdo[0x1000].raw == 0x00030191
    # 0x1018 identity: vendor, product, revision, serial, as the EDS declares.
    for sub in range(1, 5):
        assert node.sdo[0x1018][sub].raw == node.object_dictionary[0x1018][sub].default
    assert node.sdo[0x1008].raw == "Zelos PDU"
    # 0x1017 heartbeat producer time, in ms.
    assert node.sdo[0x1017].raw == 1000


def test_sdo_write_switches_outputs(node):
    # SDO works in pre-operational: write 0x6200 outputs, read back 0x6000
    # inputs and the 0x2000 per-channel currents.
    for outputs in (0b0000_0101, 0b0110_0000, 0):
        node.sdo[0x6200][1].raw = outputs
        wait_for(
            lambda: (node.sdo[0x6000][1].raw, [node.sdo[0x2000][ch].raw for ch in range(1, 9)]),
            expected(outputs),
            f"inputs and currents for outputs {outputs:#04x}",
        )


def test_rpdo_command_reflected_by_tpdos(node):
    # PDOs run only in operational. RPDO1 carries the outputs; TPDO1 the
    # inputs, TPDO2 the eight currents.
    set_state(node, "OPERATIONAL", OPERATIONAL)
    for outputs in (0b0000_0011, 0b0100_1000):
        node.rpdo[1][OUTPUTS].raw = outputs
        node.rpdo[1].transmit()
        wait_for(
            lambda: (node.tpdo[1][INPUTS].raw, [var.raw for var in node.tpdo[2]]),
            expected(outputs),
            f"TPDOs for outputs {outputs:#04x}",
        )


def test_overcurrent_trips_with_emcy_and_recovers(node):
    set_state(node, "OPERATIONAL", OPERATIONAL)
    node.emcy.reset()
    node.heartbeats.clear()

    # Switch on channel 1 and the shorted channel 8.
    node.rpdo[1][OUTPUTS].raw = 0b1000_0001
    node.rpdo[1].transmit()

    # EMCY: error code, error register, then manufacturer bytes, here the
    # stack's error bit and the tripped channel.
    emcy = wait_until(lambda: node.emcy.active, "EMCY")[0]
    assert emcy.code == EMC_CURRENT_OUTPUT
    assert emcy.register == 0x82  # manufacturer (the stack's) and current (ours)
    assert emcy.data[1:] == SHORTED.to_bytes(4, "little")

    # The error takes the node to pre-operational (0x1029, the CiA default).
    # Channel 8 is off, channel 1 stays on, and SDO still works.
    wait_until(lambda: PRE_OPERATIONAL in node.heartbeats, "pre-operational")
    assert node.sdo[0x6000][1].raw == 0b0000_0001

    # Commanding channel 8 off clears the trip: an EMCY with code 0, error reset.
    node.sdo[0x6200][1].raw = 0b0000_0001
    wait_until(lambda: node.emcy.log[-1].code == 0, "EMCY error reset")
    assert not node.emcy.active
    set_state(node, "OPERATIONAL", OPERATIONAL)


def test_communication_error_takes_error_values_and_keeps_trip(node):
    # Channel 1 on; channel 8, commanded on, trips.
    node.emcy.reset()
    node.sdo[0x6200][1].raw = 0b1000_0001
    wait_until(lambda: node.emcy.active, "trip EMCY")

    # On a communication error, every output (0x6206 defaults to all) takes its
    # error value (0x6207): here channel 2 on, the rest off.
    node.sdo[0x6207][1].raw = 0b0000_0010
    # SYNC must be empty (0x1019 is 0). One byte is a communication error,
    # EMCY 0x8240, held until reset-communication.
    node.network.send_message(0x080, b"\x00")
    wait_until(lambda: any(e.code == 0x8240 for e in node.emcy.active), "SYNC length EMCY")
    wait_for(lambda: node.sdo[0x6000][1].raw, 0b0000_0010, "error values")

    # Channel 8's error value is off, yet its trip holds: no error reset, and
    # the register still has current (0x02) with communication (0x10).
    assert all(e.code != 0 for e in node.emcy.log)
    assert node.sdo[0x1001].raw == 0x92


def test_sync_produces_tpdo3(node):
    outputs = 0b0000_0101
    node.sdo[0x6200][1].raw = outputs
    inputs, _ = expected(outputs)
    wait_for(lambda: node.sdo[0x6000][1].raw, inputs, "inputs")

    # TPDO3 is synchronous (transmission type 1): the node sends it once per
    # SYNC, and only when operational.
    set_state(node, "OPERATIONAL", OPERATIONAL)
    tpdo3 = node.tpdo[3]
    received = []
    tpdo3.add_callback(received.append)
    for count in range(1, 4):
        node.network.sync.transmit()
        wait_for(lambda: len(received), count, f"TPDO3 after SYNC {count}")
        assert tpdo3[INPUTS].raw == inputs
