# scx-slo Makefile

CC := gcc
CLANG := clang
BPFTOOL := bpftool

# Use the scx includes from the cloned repo
SCX_INCLUDE := scx/scheds/include
LIBBPF_INCLUDE := /usr/include

# Flags for BPF compilation
BPF_CFLAGS := -g -O2 -target bpf -D__TARGET_ARCH_x86_64 \
              -I$(SCX_INCLUDE) -I$(LIBBPF_INCLUDE)

# Flags for userspace compilation
CFLAGS := -g -O2 -Wall -I$(SCX_INCLUDE) -I$(LIBBPF_INCLUDE) -I$(OUT) -Iinclude -Isrc
LDFLAGS := -lbpf -lelf -lz

# Output directory
OUT := build

.PHONY: all clean test

all: $(OUT)/scx_slo

# Test targets
test: $(OUT)/test_deadline_calc $(OUT)/test_malicious_configs
	@echo "Running unit tests..."
	$(OUT)/test_deadline_calc
	$(OUT)/test_malicious_configs

# Create output directory
$(OUT):
	mkdir -p $(OUT)

# Compile BPF program
$(OUT)/scx_slo.bpf.o: src/scx_slo.bpf.c | $(OUT)
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

# Generate skeleton header
$(OUT)/scx_slo.skel.h: $(OUT)/scx_slo.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

# Compile userspace program
$(OUT)/scx_slo: src/scx_slo.c src/config.c $(OUT)/scx_slo.skel.h | $(OUT)
	$(CC) $(CFLAGS) -c src/scx_slo.c -o $(OUT)/scx_slo.o
	$(CC) $(CFLAGS) -c src/config.c -o $(OUT)/config.o
	$(CC) $(OUT)/scx_slo.o $(OUT)/config.o $(LDFLAGS) -o $@

clean:
	rm -rf $(OUT)

# For container build - just the BPF object
bpf-only: $(OUT)/scx_slo.bpf.o

# Test compilation targets
$(OUT)/test_deadline_calc: test/test_deadline_calc.c | $(OUT)
	$(CC) $(CFLAGS) $< -o $@

$(OUT)/test_malicious_configs: test/test_malicious_configs.c | $(OUT)
	$(CC) $(CFLAGS) $< -o $@

# Install target
install: $(OUT)/scx_slo
	install -m 755 $(OUT)/scx_slo /usr/local/bin/
