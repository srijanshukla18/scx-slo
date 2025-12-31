# scx-slo Makefile

CC := gcc
CLANG := clang
BPFTOOL := bpftool

# Detect architecture
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),x86_64)
    TARGET_ARCH := x86_64
    ARCH_DIR := x86
endif
ifeq ($(UNAME_M),aarch64)
    TARGET_ARCH := arm64
    ARCH_DIR := arm64
endif
ifeq ($(UNAME_M),arm64)
    TARGET_ARCH := arm64
    ARCH_DIR := arm64
endif
ifndef TARGET_ARCH
    $(error Unsupported architecture: $(UNAME_M). Supported: x86_64, aarch64)
endif

# Use the scx includes from the cloned repo
SCX_INCLUDE := scx/scheds/include
LIBBPF_INCLUDE := /usr/include

# Flags for BPF compilation
BPF_CFLAGS := -g -O2 -target bpf -D__TARGET_ARCH_$(TARGET_ARCH) \
              -I$(SCX_INCLUDE) -I$(LIBBPF_INCLUDE)

# Flags for userspace compilation
CFLAGS := -g -O2 -Wall -I$(SCX_INCLUDE) -I$(LIBBPF_INCLUDE) -I$(OUT) -Iinclude -Isrc
LDFLAGS := -lbpf -lelf -lz -lpthread

# Output directory
OUT := build

# Docker settings
IMAGE_REGISTRY ?= ghcr.io/yourorg
IMAGE_NAME := scx-slo-loader
VERSION ?= v0.1.0

# All test binaries
TEST_BINS := $(OUT)/test_deadline_calc \
             $(OUT)/test_malicious_configs \
             $(OUT)/test_config \
             $(OUT)/test_slo_main \
             $(OUT)/test_bpf_logic \
             $(OUT)/test_integration

.PHONY: all clean test test-all docker check-kernel check-deps help

all: $(OUT)/scx_slo

# Run all tests
test: $(TEST_BINS)
	@echo "Running all unit tests..."
	@echo ""
	@echo "=== test_deadline_calc ==="
	$(OUT)/test_deadline_calc
	@echo ""
	@echo "=== test_malicious_configs ==="
	$(OUT)/test_malicious_configs
	@echo ""
	@echo "=== test_config ==="
	$(OUT)/test_config
	@echo ""
	@echo "=== test_slo_main ==="
	$(OUT)/test_slo_main
	@echo ""
	@echo "=== test_bpf_logic ==="
	$(OUT)/test_bpf_logic
	@echo ""
	@echo "=== test_integration ==="
	$(OUT)/test_integration
	@echo ""
	@echo "All tests passed!"

# Alias for test
test-all: test

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

$(OUT)/test_config: test/test_config.c | $(OUT)
	$(CC) $(CFLAGS) $< -o $@

$(OUT)/test_slo_main: test/test_slo_main.c | $(OUT)
	$(CC) $(CFLAGS) $< -o $@

$(OUT)/test_bpf_logic: test/test_bpf_logic.c | $(OUT)
	$(CC) $(CFLAGS) $< -o $@

$(OUT)/test_integration: test/test_integration.c | $(OUT)
	$(CC) $(CFLAGS) $< -o $@

# Install target
install: $(OUT)/scx_slo
	install -m 755 $(OUT)/scx_slo /usr/local/bin/

# Docker build target
docker: check-deps
	docker build -t $(IMAGE_REGISTRY)/$(IMAGE_NAME):$(VERSION) -f Dockerfile .
	@echo ""
	@echo "Image built: $(IMAGE_REGISTRY)/$(IMAGE_NAME):$(VERSION)"
	@echo "To push: docker push $(IMAGE_REGISTRY)/$(IMAGE_NAME):$(VERSION)"

# Check kernel version (only works on Linux)
check-kernel:
	@if [ "$$(uname -s)" = "Linux" ]; then \
		KVER=$$(uname -r | cut -d. -f1-2); \
		MAJOR=$$(echo $$KVER | cut -d. -f1); \
		MINOR=$$(echo $$KVER | cut -d. -f2); \
		if [ "$$MAJOR" -lt 6 ] || ([ "$$MAJOR" -eq 6 ] && [ "$$MINOR" -lt 12 ]); then \
			echo "WARNING: Kernel $$KVER is below 6.12. sched_ext requires Linux 6.12+"; \
			exit 1; \
		else \
			echo "Kernel $$KVER OK (>= 6.12)"; \
		fi; \
		if [ -f /sys/kernel/sched_ext/state ]; then \
			echo "sched_ext state: $$(cat /sys/kernel/sched_ext/state)"; \
		else \
			echo "WARNING: /sys/kernel/sched_ext/state not found. CONFIG_SCHED_CLASS_EXT may not be enabled."; \
		fi; \
	else \
		echo "Not running on Linux. Skipping kernel check."; \
	fi

# Check build dependencies
check-deps:
	@echo "Checking build dependencies..."
	@command -v $(CLANG) >/dev/null 2>&1 || { echo "ERROR: clang not found"; exit 1; }
	@command -v $(BPFTOOL) >/dev/null 2>&1 || { echo "ERROR: bpftool not found"; exit 1; }
	@command -v $(CC) >/dev/null 2>&1 || { echo "ERROR: gcc not found"; exit 1; }
	@test -d $(SCX_INCLUDE) || { echo "ERROR: scx includes not found at $(SCX_INCLUDE). Run: git clone https://github.com/sched-ext/scx.git"; exit 1; }
	@echo "All dependencies found."

# Help target
help:
	@echo "scx-slo Makefile targets:"
	@echo ""
	@echo "  make           - Build the scheduler binary"
	@echo "  make test      - Run all unit tests"
	@echo "  make docker    - Build Docker container image"
	@echo "  make install   - Install binary to /usr/local/bin"
	@echo "  make clean     - Remove build artifacts"
	@echo "  make check-kernel - Verify kernel supports sched_ext"
	@echo "  make check-deps   - Verify build dependencies"
	@echo "  make bpf-only  - Build only the BPF object file"
	@echo ""
	@echo "Configuration variables:"
	@echo "  IMAGE_REGISTRY=$(IMAGE_REGISTRY)"
	@echo "  IMAGE_NAME=$(IMAGE_NAME)"
	@echo "  VERSION=$(VERSION)"
	@echo "  TARGET_ARCH=$(TARGET_ARCH)"
