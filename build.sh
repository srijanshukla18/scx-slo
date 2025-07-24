#!/bin/bash
# Build script for scx-slo scheduler
set -e

# Configuration
IMAGE_REGISTRY=${IMAGE_REGISTRY:-"ghcr.io/yourorg"}
IMAGE_NAME="scx-slo-loader"
VERSION=${VERSION:-"v0.1.0"}

echo "Building scx-slo scheduler..."

# Build the container image
docker build -t ${IMAGE_REGISTRY}/${IMAGE_NAME}:${VERSION} -f Dockerfile .

echo "Build complete!"
echo ""
echo "To push the image:"
echo "  docker push ${IMAGE_REGISTRY}/${IMAGE_NAME}:${VERSION}"
echo ""
echo "To deploy to Kubernetes:"
echo "  1. Update the image in scx-slo-daemonset.yaml"
echo "  2. kubectl apply -f scx-slo-daemonset.yaml"
echo ""
echo "To test with demo workloads:"
echo "  kubectl apply -f demo-workloads.yaml"