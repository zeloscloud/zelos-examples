# The Zelos agent, with the CAN extension installed.
#
# Installing the extension at build time rather than at run time keeps the
# download, the Python interpreter and its dependencies in an image layer.
FROM ubuntu:22.04

ARG AGENT_VERSION=26.0.9
ARG CLI_VERSION=0.1.10
# The marketplace identifier is the extension's repository.
ARG CAN_EXTENSION=zeloscloud/zelos-extension-can
ARG CAN_EXTENSION_VERSION=v0.1.17

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl procps \
 && curl -fsSLo /usr/share/keyrings/zelos-archive-keyring.gpg \
      https://release.zeloscloud.io/app/zelos-archive-keyring.gpg \
 && printf '%s\n' 'Types: deb' 'URIs: https://release.zeloscloud.io/app/ubuntu' 'Suites: jammy' \
      'Components: main' 'Signed-By: /usr/share/keyrings/zelos-archive-keyring.gpg' \
      > /etc/apt/sources.list.d/zelos.sources \
 && apt-get update \
 && apt-get install -y --no-install-recommends "zelos-agent=${AGENT_VERSION}" \
 && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL "https://release.zeloscloud.io/cli/${CLI_VERSION}/zelos-x86_64-unknown-linux-gnu.tar.gz" \
      | tar xz -C /usr/local/bin zelos \
 && zelos --version

# Installing an extension goes through a running agent, so start a throwaway
# one here.
RUN set -eux; \
    zelos-agent --store-type memory --quiet & \
    agent_pid=$!; \
    for _ in $(seq 60); do \
        if zelos extensions list >/dev/null 2>&1; then break; fi; \
        sleep 1; \
    done; \
    zelos extensions install "${CAN_EXTENSION}" "${CAN_EXTENSION_VERSION}"; \
    kill "${agent_pid}"; \
    wait "${agent_pid}" 2>/dev/null || true

# Starts the agent and the extension with the bench's configuration, mounted at
# /bench/can.json by the bench that runs it.
COPY agent-entrypoint.sh /usr/local/bin/bench-agent
CMD ["bash", "/usr/local/bin/bench-agent"]
