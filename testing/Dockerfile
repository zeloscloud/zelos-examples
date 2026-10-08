# The bench's tester: testing/, installed with its locked dependencies. Runs
# pytest on the bench's bus.
#
# dbc/ is mounted at /dbc, which is where the package looks for it relative to
# /testing.
FROM python:3.12-slim

COPY --from=ghcr.io/astral-sh/uv:0.8.22 /uv /usr/local/bin/uv

# `ip` reads the channel's controller state for the bus_health fixture.
RUN apt-get update \
 && apt-get install -y --no-install-recommends iproute2 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /testing
ENV UV_PYTHON_DOWNLOADS=never

# Dependencies first, so editing a test does not reinstall them.
COPY pyproject.toml uv.lock ./
RUN uv sync --frozen --no-install-project
COPY src src
COPY suites suites
RUN uv sync --frozen

ENTRYPOINT ["uv", "run", "--frozen", "pytest"]
