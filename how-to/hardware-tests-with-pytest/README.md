# Run bench tests with pytest and open the failure

Code for [Run bench tests with pytest and open the failure](https://zeloscloud.io/blog/hardware-tests-with-pytest).

Install the SDK with `pip install "zelos-sdk[test]"`, start the Zelos app and `zelos live demo --backfill 5m --duration 30m`, then:

```bash
pytest
```
