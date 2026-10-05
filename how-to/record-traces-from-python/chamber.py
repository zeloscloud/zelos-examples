import time
from datetime import datetime
import zelos_sdk

zelos_sdk.init()
chamber = zelos_sdk.TraceSource("chamber")

name = f"chamber_{datetime.now():%Y%m%d_%H%M%S}.trz"
with zelos_sdk.TraceWriter(name):
    chamber.log("setup", {"operator": "dana", "firmware": "1.4.2", "setpoint_c": 45.0})
    for i in range(200):
        chamber.log("status", {"temp_c": 25.0 + i * 0.1, "door_closed": True})
        time.sleep(0.05)
