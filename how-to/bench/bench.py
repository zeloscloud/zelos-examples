import time

from zelos_sdk import Agent, DataType, TraceEventFieldMetadata, TraceSource, action, init
from zelos_sdk.actions import ActionExecuteResult

VOLTS = {"3v3": 3.28, "5v0": 5.02, "12v0": 11.95}  # stand-in for your instrument
enabled = {"3v3": True, "5v0": True, "12v0": False}
CHANNELS = {"supply": ["ch1", "ch2"], "dmm": ["dcv", "acv"]}
limit = {"volts": 3.0}  # the alarm threshold a custom tab sets
agent = Agent()  # lazy; used only by read_cell_voltage


def channels_for(instrument=None):
    return CHANNELS.get(instrument, [])


@action("Read Voltage", "Read one power rail", read_only=True)
@action.select("rail", choices=list(VOLTS))
def read_voltage(rail: str):
    if not enabled[rail]:
        return ActionExecuteResult.failed(f"{rail} is off")
    return ActionExecuteResult.passed({"rail": rail, "voltage_v": VOLTS[rail]})


@action("Set Rail", "Turn one power rail on or off", submit_text="Apply")
@action.select("rail", choices=list(VOLTS))
@action.boolean("on", default=True)
def set_rail(rail: str, on: bool = True):
    enabled[rail] = on
    return {"rail": rail, "on": on}


@action("Read Channel", "Read one channel of one instrument", read_only=True)
@action.select("instrument", choices=list(CHANNELS))
@action.select("channel", choices=channels_for, depends_on="instrument")
def read_channel(instrument: str, channel: str):
    return {"instrument": instrument, "channel": channel, "value": 0.0}


@action("Read Cell Voltage", "Latest voltage of one pack cell", read_only=True)
@action.select("cell", choices=[f"cell_{i}" for i in range(8)])
def read_cell_voltage(cell: str):
    # needs `zelos live demo` running; without it the result is ERROR
    return agent.latest(f"bus0/BMS_message/cells.{cell}").value


# A custom tab sets this, then reads it back from limits/threshold.value
@action("Set Threshold", "Set the low-cell alarm threshold")
@action.number("volts", minimum=2.5, maximum=4.2, default=3.0)
def set_threshold(volts: float = 3.0):
    limit["volts"] = volts
    return {"volts": volts}


init("bench", actions=True)

# Stream the rails and the threshold at 10 Hz
rails = TraceSource("rails").add_event(
    "voltage", [TraceEventFieldMetadata(r, DataType.Float64, "V") for r in VOLTS])
threshold = TraceSource("limits").add_event(
    "threshold", [TraceEventFieldMetadata("value", DataType.Float64, "V")])
try:
    while True:
        rails.log(**{r: VOLTS[r] if enabled[r] else 0.0 for r in VOLTS})
        threshold.log(value=limit["volts"])
        time.sleep(0.1)
except KeyboardInterrupt:
    pass
