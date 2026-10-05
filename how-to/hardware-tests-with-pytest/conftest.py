from datetime import datetime, timedelta, timezone
import pytest

@pytest.fixture(autouse=True)
def bench_trace(request, agent):
    start = datetime.now(timezone.utc) - timedelta(seconds=60)
    yield
    path = request.config.zelos_local_artifacts_dir / f"{request.node.name}.trz"
    agent.export(str(path), start=start, overwrite=True)
