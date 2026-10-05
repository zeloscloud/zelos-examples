import math
import time

import zelos_sdk

zelos_sdk.init()
supply = zelos_sdk.TraceSource("bench_supply")

while True:
    t = time.time()
    voltage = 12.0 + 0.5 * math.sin(t)
    current = 2.0 + 0.2 * math.sin(t)
    supply.log("output", {"voltage": voltage, "current": current})
    time.sleep(0.01)  # 10 ms between readings
