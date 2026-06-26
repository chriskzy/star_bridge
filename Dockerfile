# ------------------------------------------------------------------ #
#  Multi-stage Dockerfile for star_bridge                              #
#  Builds the bridge binary and runs it in a minimal runtime image     #
# ------------------------------------------------------------------ #

# ---------- Stage 1: Build ----------
FROM ubuntu:24.04 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    clang \
    libcjson-dev \
    zlib1g-dev \
    python3 \
    python3-pip \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy source tree
COPY include/ include/
COPY src/ src/
COPY vendor/ vendor/
COPY Makefile .
COPY scripts/ scripts/

# Build the bridge binary
RUN make clean && make

# ---------- Stage 2: Runtime ----------
FROM ubuntu:24.04 AS runtime

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libcjson-dev \
    zlib1g \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy only the built binary and scripts
COPY --from=builder /app/bin/star_bridge bin/star_bridge
COPY --from=builder /app/scripts/ scripts/
COPY --from=builder /app/Makefile .

# Default config path (can be overridden at runtime)
ENV CODEX_CONFIG_PATH=/app/config/codex_config.json
ENV CODEX_PORT=8080

# Expose bridge port
EXPOSE 8080

# Entry point
CMD ["bin/star_bridge"]
