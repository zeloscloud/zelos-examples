#!/bin/bash
# Start the agent and hand an extension its configuration: the CAN extension
# unless BENCH_EXTENSION names another.
#
# The agent owns the extension from then on: its process, its restarts and its
# exit codes. Configuring it here also avoids a limitation of the app, which
# disables extension installs while more than one agent is connected.
set -euo pipefail

CONFIG=${BENCH_CONFIG:-/bench/can.json}
EXTENSION=${BENCH_EXTENSION:-zeloscloud.zelos-extension-can}
# A directory holding an extension's source, for one not yet on the
# marketplace. Installed at every start, which took 12 to 42 s on the bench.
LOCAL=${BENCH_EXTENSION_LOCAL:-}

# zelos-agent has no --version; it logs its version on startup instead.
echo "cli:       $(zelos --version 2>&1 | head -1)"
echo "extension: ${EXTENSION}"
echo "config:    ${CONFIG}"

# A memory store, explicitly: the default keeps the signal catalog and discards
# every sample, so queries return nothing.
zelos-agent \
    --store-type memory \
    --store-retain-duration "${BENCH_RETAIN:-15m}" &
agent_pid=$!

trap 'kill "${agent_pid}" 2>/dev/null || true' TERM INT

# Wait for a real API call, not just for the port to open: zelos status can
# succeed while the agent is still starting its services.
ready=no
for attempt in $(seq 60); do
    if ! kill -0 "${agent_pid}" 2>/dev/null; then
        echo "agent exited during startup" >&2
        wait "${agent_pid}" || true
        exit 1
    fi
    if zelos extensions list >/dev/null 2>&1; then
        echo "agent answered on attempt ${attempt}"
        ready=yes
        break
    fi
    sleep 1
done

if [ "${ready}" != yes ]; then
    echo "agent did not answer within 60s" >&2
    exit 1
fi

# install-local runs the extension from that directory and builds its
# environment under /var/lib/zelos-agent, so the mount can be read-only.
if [ -n "${LOCAL}" ]; then
    zelos extensions install-local "${LOCAL}"
fi

# Retried because it is the first call that does real work.
started=no
for attempt in $(seq 10); do
    if zelos extensions start "${EXTENSION}" --config-file "${CONFIG}"; then
        started=yes
        break
    fi
    echo "extension start failed (attempt ${attempt}), retrying" >&2
    sleep 2
done

if [ "${started}" != yes ]; then
    echo "could not start ${EXTENSION}" >&2
    exit 1
fi

# The agent is the long-running process; keep this container's lifetime tied to it.
wait "${agent_pid}"
