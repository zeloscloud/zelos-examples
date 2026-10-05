import pytest

@pytest.mark.parametrize("cell", range(8))
def test_cell_above_floor(agent, check, cell):
    voltage = agent.signal(f"bus0/BMS_message/cells.cell_{cell}")
    check.that(voltage, ">", 3.0, last="60s")
