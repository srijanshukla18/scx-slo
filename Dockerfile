# scx-slo deployment image
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
    && rm -rf /var/lib/apt/lists/*

# Clone scx to get headers and build system
WORKDIR /tmp
RUN git clone https://github.com/sched-ext/scx.git

# Copy our scheduler source
WORKDIR /build
COPY src/scx_slo.bpf.c ./
COPY Makefile ./

# Create simplified Makefile for just BPF compilation
RUN echo 'scx_slo.bpf.o: scx_slo.bpf.c' > Makefile.simple && \
    echo '\tclang -g -O2 -target bpf -D__TARGET_ARCH_x86_64 \\' >> Makefile.simple && \
    echo '\t  -I/tmp/scx/scheds/include -I/usr/include/bpf \\' >> Makefile.simple && \
    echo '\t  -c scx_slo.bpf.c -o scx_slo.bpf.o' >> Makefile.simple

# Build the BPF object
RUN make -f Makefile.simple scx_slo.bpf.o

# Build scx_loader from scx project
WORKDIR /tmp/scx
RUN apt-get update && apt-get install -y meson ninja-build python3-pip && \
    meson setup build --prefix /usr && \
    cd build && \
    ninja -C . scheds/rust/scx_loader/scx_loader

# Final minimal image
FROM scratch
COPY --from=builder /build/scx_slo.bpf.o /opt/scx_slo.bpf.o
COPY --from=builder /tmp/scx/build/scheds/rust/scx_loader/scx_loader /usr/bin/scx_loader
ENTRYPOINT ["/usr/bin/scx_loader", "/opt/scx_slo.bpf.o"]