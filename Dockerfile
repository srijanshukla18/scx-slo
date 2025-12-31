# scx-slo deployment image
# Multi-stage build for production deployment

# =============================================================================
# Stage 1: Build environment
# =============================================================================
FROM ubuntu:24.04 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    clang \
    llvm \
    gcc \
    make \
    libbpf-dev \
    libelf-dev \
    linux-headers-generic \
    pkg-config \
    git \
    bpftool \
    cargo \
    rustc \
    meson \
    ninja-build \
    python3-pip \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Clone scx to get headers and build system
# Pin to a specific commit for reproducible builds
WORKDIR /tmp
ARG SCX_VERSION=v1.0.8
RUN git clone --depth=1 --branch ${SCX_VERSION} https://github.com/sched-ext/scx.git || \
    git clone --depth=1 https://github.com/sched-ext/scx.git

# Copy our scheduler source
WORKDIR /build
COPY src/ ./src/
COPY include/ ./include/
COPY Makefile ./

# Detect target architecture
ARG TARGETARCH=amd64
RUN if [ "$TARGETARCH" = "amd64" ] || [ "$TARGETARCH" = "x86_64" ]; then \
        export BPF_TARGET_ARCH=x86_64; \
    elif [ "$TARGETARCH" = "arm64" ] || [ "$TARGETARCH" = "aarch64" ]; then \
        export BPF_TARGET_ARCH=arm64; \
    else \
        export BPF_TARGET_ARCH=x86_64; \
    fi && \
    echo "Building for architecture: $BPF_TARGET_ARCH" && \
    clang -g -O2 -target bpf -D__TARGET_ARCH_${BPF_TARGET_ARCH} \
        -I/tmp/scx/scheds/include \
        -I/usr/include/bpf \
        -c src/scx_slo.bpf.c -o scx_slo.bpf.o

# Build the full userspace binary
RUN bpftool gen skeleton scx_slo.bpf.o > scx_slo.skel.h && \
    gcc -g -O2 -Wall \
        -I/tmp/scx/scheds/include \
        -I/usr/include \
        -I. \
        -Iinclude \
        -Isrc \
        -c src/scx_slo.c -o scx_slo.o && \
    gcc -g -O2 -Wall \
        -I/tmp/scx/scheds/include \
        -I/usr/include \
        -I. \
        -Iinclude \
        -Isrc \
        -c src/config.c -o config.o && \
    gcc scx_slo.o config.o -lbpf -lelf -lz -o scx_slo

# =============================================================================
# Stage 2: K8s Watcher (Go)
# =============================================================================
FROM golang:1.22-bookworm AS go-builder
WORKDIR /build
COPY src/k8s-watcher/ ./
RUN go mod init k8s-watcher && \
    go get github.com/cilium/ebpf k8s.io/client-go/... && \
    go build -o k8s-watcher main.go

# =============================================================================
# Stage 3: Runtime image
# =============================================================================
FROM debian:bookworm-slim AS runtime

# Install minimal runtime dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    libbpf1 \
    libelf1 \
    zlib1g \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Copy built artifacts
COPY --from=builder /build/scx_slo /usr/bin/scx_slo
COPY --from=builder /build/scx_slo.bpf.o /opt/scx-slo/scx_slo.bpf.o
COPY --from=go-builder /build/k8s-watcher /usr/bin/k8s-watcher

# Create config directory
RUN mkdir -p /etc/scx-slo && chown scx-slo:scx-slo /etc/scx-slo

# Set permissions
RUN chmod 755 /usr/bin/scx_slo

# Health check script
COPY --chmod=755 <<'EOF' /usr/local/bin/healthcheck.sh
#!/bin/sh
if [ -f /sys/kernel/sched_ext/state ]; then
    STATE=$(cat /sys/kernel/sched_ext/state)
    if [ "$STATE" = "enabled" ]; then
        OPS=$(cat /sys/kernel/sched_ext/*/ops 2>/dev/null || echo "unknown")
        if echo "$OPS" | grep -q "scx_slo"; then
            exit 0
        fi
    fi
fi
exit 1
EOF

# Metadata
LABEL org.opencontainers.image.title="scx-slo"
LABEL org.opencontainers.image.description="SLO-aware eBPF CPU scheduler"
LABEL org.opencontainers.image.source="https://github.com/sched-ext/scx-slo"
LABEL org.opencontainers.image.licenses="GPL-2.0"

# Default entrypoint runs the scheduler with verbose mode and config reload
ENTRYPOINT ["/usr/bin/scx_slo"]
CMD ["-v", "-c"]
