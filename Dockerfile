# scx-slo deployment image
# Multi-stage build for production deployment

# =============================================================================
# Stage 1: Build environment
# =============================================================================
FROM ubuntu:24.04 AS builder

# Install build dependencies
# Note: We build libbpf and bpftool from source because the system bpftool
# doesn't properly generate struct_ops skeletons needed for sched_ext
RUN apt-get update && apt-get install -y \
    clang \
    llvm \
    gcc \
    make \
    libelf-dev \
    libssl-dev \
    pkg-config \
    git \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Clone scx for headers
WORKDIR /tmp
ARG SCX_VERSION=v1.0.8
RUN git clone --depth=1 --branch ${SCX_VERSION} https://github.com/sched-ext/scx.git || \
    git clone --depth=1 https://github.com/sched-ext/scx.git

# Clone and build libbpf (needed for struct_ops skeleton generation)
RUN git clone --depth=1 https://github.com/libbpf/libbpf.git
WORKDIR /tmp/libbpf/src
RUN make -j$(nproc) && make install PREFIX=/usr/local

# Clone and build bpftool (with struct_ops support)
WORKDIR /tmp
RUN git clone --recurse-submodules https://github.com/libbpf/bpftool.git
WORKDIR /tmp/bpftool/src
RUN make -j$(nproc) && cp bpftool /usr/local/bin/

# Ensure libbpf is in library path
ENV LD_LIBRARY_PATH=/usr/local/lib64:/usr/local/lib:$LD_LIBRARY_PATH
ENV PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig:/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH

# Copy our scheduler source
WORKDIR /build
COPY src/ ./src/
COPY include/ ./include/
COPY Makefile ./

# Detect target architecture (vmlinux.h from scx provides all kernel types)
ARG TARGETARCH=amd64
RUN if [ "$TARGETARCH" = "amd64" ] || [ "$TARGETARCH" = "x86_64" ]; then \
        export BPF_TARGET_ARCH=x86_64; \
    elif [ "$TARGETARCH" = "arm64" ] || [ "$TARGETARCH" = "aarch64" ]; then \
        export BPF_TARGET_ARCH=arm64; \
    else \
        export BPF_TARGET_ARCH=x86_64; \
    fi && \
    echo "Building BPF for architecture: $BPF_TARGET_ARCH" && \
    clang -g -O2 -target bpf -mcpu=v3 -D__TARGET_ARCH_${BPF_TARGET_ARCH} \
        -I/tmp/scx/scheds/include \
        -I/tmp/scx/scheds/include/arch/${BPF_TARGET_ARCH} \
        -I/tmp/scx/scheds/include/bpf-compat \
        -I/usr/local/include \
        -c src/scx_slo.bpf.c -o scx_slo.bpf.o

# Generate skeleton and build userspace binary
# Using locally built bpftool which properly supports struct_ops
RUN /usr/local/bin/bpftool gen skeleton scx_slo.bpf.o > scx_slo.skel.h && \
    gcc -g -O2 -Wall \
        -I/tmp/scx/scheds/include \
        -I/usr/local/include \
        -I. \
        -Iinclude \
        -Isrc \
        -c src/scx_slo.c -o scx_slo.o && \
    gcc -g -O2 -Wall \
        -I/tmp/scx/scheds/include \
        -I/usr/local/include \
        -I. \
        -Iinclude \
        -Isrc \
        -c src/config.c -o config.o && \
    gcc scx_slo.o config.o -L/usr/local/lib64 -L/usr/local/lib -lbpf -lelf -lz -o scx_slo

# =============================================================================
# Stage 2: K8s Watcher (Go)
# =============================================================================
FROM golang:1.24-bookworm AS go-builder
WORKDIR /build
COPY src/k8s-watcher/go.mod src/k8s-watcher/go.sum ./
RUN go mod download
COPY src/k8s-watcher/ ./
RUN go build -o k8s-watcher main.go

# =============================================================================
# Stage 3: Runtime image
# =============================================================================
FROM debian:bookworm-slim AS runtime

# Install minimal runtime dependencies
# Note: We copy libbpf from builder stage instead of using system libbpf
RUN apt-get update && apt-get install -y --no-install-recommends \
    libelf1 \
    zlib1g \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Copy libbpf from builder
COPY --from=builder /usr/local/lib64/libbpf* /usr/local/lib64/
RUN ldconfig /usr/local/lib64

# Create dedicated user/group for least-privilege runtime
RUN addgroup --system scx-slo && adduser --system --ingroup scx-slo scx-slo

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
