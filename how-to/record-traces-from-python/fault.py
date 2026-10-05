from datetime import datetime
from pathlib import Path
from zelos_sdk import connect

agent = connect()
cell = "bus0/BMS_message/cells.cell_3"

for tick in agent.watch([cell], interval=1.0):
    if tick[cell].value < 3.0:
        out = Path(f"fault_{datetime.now():%Y%m%d_%H%M%S}.trz").resolve()
        result = agent.export(out, start="-2m")
        print(out.name, result.ok)
        break

with agent.trace(str(out)) as trace:
    frame = trace.query(cell)
    print(frame[cell].min())
