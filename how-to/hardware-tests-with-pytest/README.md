# Run bench tests with pytest and open the failure

Code for [Run bench tests with pytest and open the failure](https://zeloscloud.io/blog/hardware-tests-with-pytest).

This folder is the project the post makes with `uv init bench` and `uv add "zelos-sdk[test]"`. Install [uv](https://docs.astral.sh/uv/), start the Zelos app and `zelos live demo --backfill 5m --duration 30m`, then run in this folder:

```bash
uv run pytest
```
