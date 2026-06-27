# ------------------------------------------------------------------ #
#  Multi-stage Dockerfile for star_bridge                              #
#  Builds the bridge binary and runs it in a minimal runtime image.    #
#                                                                      #
#  The default command runs a self-contained DEMO bridge backed by a   #
#  bundled fake agent, so `docker run` / `docker compose up` work out  #
#  of the box and the healthcheck passes. A real ds4 agent (which       #
#  needs a model file that does not ship in the image) is supplied by   #
#  overriding the command — see the examples at the bottom.            #
# ------------------------------------------------------------------ #

# ---------- Stage 1: Build ----------
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    clang \
    libcjson-dev \
    zlib1g-dev \
    python3 \
    python3-venv \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY include/ include/
COPY src/ src/
COPY vendor/ vendor/
COPY Makefile .
COPY scripts/ scripts/

# Build only the binary here (avoid the venv target, which is a runtime concern).
RUN make clean && make bin/star_bridge

# ---------- Stage 2: Runtime ----------
FROM ubuntu:24.04 AS runtime

# Runtime deps: zlib/cjson for the binary, python3 + venv for the ds4 wrapper,
# curl for the healthcheck.
RUN apt-get update && apt-get install -y \
    libcjson1 \
    zlib1g \
    ca-certificates \
    curl \
    python3 \
    python3-venv \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Binary + scripts + Makefile.
COPY --from=builder /app/bin/star_bridge bin/star_bridge
RUN ln -sf star_bridge bin/codex_bridge
COPY scripts/ scripts/
COPY Makefile .

# The Python wrapper the README documents for real ds4-agent mode, plus a bundled
# fake agent so the default demo command works without a model.
COPY agent/ agent/
COPY tests/fake_agent.py tests/fake_agent.sh tests/

# Build the wrapper virtualenv (falls back to the available python3).
RUN bash scripts/setup_python_venv.sh && chmod +x tests/fake_agent.sh

ENV CODEX_PORT=8080
EXPOSE 8080

# ENTRYPOINT is the bridge binary; CMD provides default args (the demo agent).
# Override CMD to point at a real agent + workspace.
ENTRYPOINT ["/app/bin/star_bridge"]
CMD ["tests/fake_agent.sh", "/app", "-p", "8080", "--framed"]

# ------------------------------------------------------------------ #
#  Examples                                                            #
#                                                                      #
#  Demo (bundled fake agent):                                          #
#    docker build -t star-bridge .                                     #
#    docker run --rm -p 8080:8080 star-bridge                          #
#    curl http://localhost:8080/v1/models                             #
#                                                                      #
#  Real ds4 agent (mount the agent + model, then override the command):#
#    docker run --rm -p 9033:9033 \                                    #
#      -v /path/to/ds4-agent:/agent/ds4-agent \                        #
#      -v /path/to/workspace:/workspace \                              #
#      star-bridge /agent/ds4-agent /workspace -p 9033 --framed        #
# ------------------------------------------------------------------ #
