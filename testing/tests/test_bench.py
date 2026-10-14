"""BenchQuiet against a fake actions client, and the bench helpers: no agent, no bus.

    cd testing && uv run pytest tests --interface virtual
"""

from types import SimpleNamespace

import can
import pytest

from zelos_testing.bench import (
    READ_ACTION,
    WRITE_ACTION,
    BenchQuiet,
    BenchQuietError,
    QuietConfig,
    foreign_canopen,
    tpdo_per_sync,
)

CFG = QuietConfig(agent="http://plc-agent:2300", device="codesys/plc")


class FakeActions:
    """The PLC's register behind zelos_sdk's Agent.actions; fail_writes fails writes of those values."""

    def __init__(self, value, fail_writes=()):
        self.value, self.fail_writes, self.writes = value, set(fail_writes), []

    def execute(self, action, params, timeout=None):
        assert params["device"] == CFG.device and params["name"] == CFG.register
        if action == READ_ACTION:
            return SimpleNamespace(ok=True, value={"value": self.value, "success": True})
        assert action == WRITE_ACTION
        if params["value"] in self.fail_writes:
            return SimpleNamespace(ok=True, value={"error": "no response from device", "success": False})
        self.writes.append(params["value"])
        self.value = params["value"]
        return SimpleNamespace(ok=True, value={"value": self.value, "success": True})


def test_writes_quiet_and_restores_the_snapshot():
    # Other bits (the VCU's mute) are left as found.
    client = FakeActions(0b01)
    with BenchQuiet(CFG, client):
        assert client.value == 0b11
    assert client.writes == [0b11, 0b01]


def test_restores_after_a_failing_test():
    client = FakeActions(0)
    with pytest.raises(KeyboardInterrupt), BenchQuiet(CFG, client):
        raise KeyboardInterrupt
    assert client.writes == [CFG.mask, 0]


def test_already_quiet_is_left_quiet():
    client = FakeActions(CFG.mask)
    with BenchQuiet(CFG, client):
        pass
    assert client.writes == []


def test_restore_failure_names_the_manual_command(caplog):
    client = FakeActions(0, fail_writes={0})
    with pytest.raises(BenchQuietError, match="restore it by hand") as err, BenchQuiet(CFG, client):
        pass
    command = CFG.manual_restore(0)
    assert command in str(err.value) and command in caplog.text
    assert '"value": 0' in command


@pytest.mark.parametrize(("state", "present"), [(0x7F, False), (0x05, True)])
def test_an_operational_pdu_is_a_foreign_manager(state, present):
    # Fail closed: only a manager starts the PDU, even one not heard.
    with can.Bus(interface="virtual", channel="bench") as rx, can.Bus(interface="virtual", channel="bench") as tx:
        tx.send(can.Message(arbitration_id=0x720, data=[state], is_extended_id=False))
        assert foreign_canopen(rx, window_s=0.2).present is present


def test_tpdo_per_sync_leaves_out_a_manager_reset():
    sync, tpdo, hb = (0x080, b""), (0x3A0, b"\x00"), 0x720
    frames = [sync, tpdo, sync, tpdo, sync, (hb, b"\x7f"), (0x000, b"\x81\x20"), sync, sync, (hb, b"\x05"), sync, sync, tpdo, sync]
    # The interval the node left, and those until its operational heartbeat, are not counted.
    assert tpdo_per_sync([(i, d, 0.0) for i, d in frames], 0x20, 0x3A0) == ([1, 1, 0, 1], 3)
