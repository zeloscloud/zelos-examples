# Record a trace file from your own Python script

Code for [Record a trace file from your own Python script](https://zeloscloud.io/blog/record-traces-from-python).

Install [uv](https://docs.astral.sh/uv/), start the Zelos app and `zelos live demo --backfill 5m --duration 30m`, then:

```bash
uv run --with zelos-sdk fault.py
```
