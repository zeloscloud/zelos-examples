import math
import time
from enum import IntEnum

import zelos_sdk
from zelos_sdk import DataType, TraceEventFieldMetadata as Field


class PackState(IntEnum):
    IDLE = 0
    PRECHARGE = 1
    DRIVE = 2
    CHARGING = 3


zelos_sdk.init()
source = zelos_sdk.TraceSource("pack")
status = source.add_event("status", [
    Field("voltage", DataType.Float64, "V"),
    Field("temperature", DataType.Float32, "°C"),
    Field("state", DataType.UInt8),
])
source.add_value_table("status", "state", {s.value: s.name for s in PackState})

start = time.time()
while True:
    t = time.time() - start
    status.log(
        voltage=380 + 20 * math.sin(t / 5),
        temperature=30 + 5 * math.sin(t / 20),
        state=PackState(int(t // 5) % 4),
    )
    time.sleep(0.1)
