---
requires-python: '>=3.10'
dependencies:
- zelos-sdk[notebook]
---

```python {#b045a676}
from zelos_sdk import connect

agent = connect()
agent
```

Do all eight cells hold their voltage through a discharge cycle?

```python {#0p536hpm}
cells = agent.query("bus0/BMS_message/cells.*", start="-2m")
cells
```

```python {#qjc178ok}
cells.short_names().plot(title="Cell voltages, last 2 minutes")
```
