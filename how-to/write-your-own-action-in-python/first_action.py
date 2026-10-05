from zelos_sdk import action, init

@action("Read Voltage", "Read one power rail", read_only=True)
@action.select("rail", choices=["3v3", "5v0", "12v0"])
def read_voltage(rail: str):
    volts = {"3v3": 3.28, "5v0": 5.02, "12v0": 11.95}
    return {"rail": rail, "voltage_v": volts[rail]}

init("bench", actions=True, block=True)
