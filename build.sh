#!/bin/bash
# Build script for scx-slo scheduler
# Usage: ./build.sh [--push] [--multi-arch]
set -euo pipefail

# Configuration
IMAGE_REGISTRY="${IMAGE_REGISTRY:-ghcr.io/yourorg}"
IMAGE_NAME="scx-slo-loader"
VERSION="${VERSION:-v0.1.0}"
FULL_IMAGE="${IMAGE_REGISTRY}/${IMAGE_NAME}:${VERSION}"

# Parse arguments
PUSH=false
MULTI_ARCH=false
for arg in "$@"; do
    case $arg in
        --push)
            PUSH=true
            shift
            ;;
        --multi-arch)
            MULTI_ARCH=true
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --push        Push image to registry after build"
            echo "  --multi-arch  Build for both amd64 and arm64"
            echo "  --help        Show this help message"
            echo ""
            echo "Environment variables:"
            echo "  IMAGE_REGISTRY  Container registry (default: ghcr.io/yourorg)"
            echo "  VERSION         Image version tag (default: v0.1.0)"
            exit 0
            ;;
    esac
done

echo "=== scx-slo Container Build ==="
echo "Image: ${FULL_IMAGE}"
echo ""

# Check for Docker or Podman
if command -v docker &> /dev/null; then
    CONTAINER_CMD="docker"
elif command -v podman &> /dev/null; then
    CONTAINER_CMD="podman"
else
    echo "ERROR: Neither docker nor podman found. Please install one."
    exit 1
fi

echo "Using: ${CONTAINER_CMD}"

# Build the image
if [ "$MULTI_ARCH" = true ]; then
    echo "Building multi-architecture image (amd64 + arm64)..."
    ${CONTAINER_CMD} buildx build \
        --platform linux/amd64,linux/arm64 \
        -t "${FULL_IMAGE}" \
        -f Dockerfile \
        ${PUSH:+--push} \
        .
else
    echo "Building single-architecture image..."
    ${CONTAINER_CMD} build \
        -t "${FULL_IMAGE}" \
        -f Dockerfile \
        .

    if [ "$PUSH" = true ]; then
        echo ""
        echo "Pushing image..."
        ${CONTAINER_CMD} push "${FULL_IMAGE}"
    fi
fi

echo ""
echo "=== Build Complete ==="
echo ""
echo "Image: ${FULL_IMAGE}"
echo ""

if [ "$PUSH" = false ]; then
    echo "To push the image:"
    echo "  ${CONTAINER_CMD} push ${FULL_IMAGE}"
    echo ""
fi

echo "To deploy to Kubernetes:"
echo "  1. Update image reference in scx-slo-daemonset.yaml if needed"
echo "  2. kubectl apply -f scx-slo-daemonset.yaml"
echo ""
echo "To test with demo workloads:"
echo "  kubectl apply -f demo-workloads.yaml"
