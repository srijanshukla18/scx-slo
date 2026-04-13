# Lab Guide: scx-slo on AWS

This guide walks through deploying and testing scx-slo on AWS infrastructure.

## Prerequisites

**Tools:**
```bash
# AWS CLI
brew install awscli
aws configure

# Kubernetes tools
brew install kubectl eksctl

# Optional: k9s for cluster monitoring
brew install k9s
```

**AWS Requirements:**
- AWS account with EC2 and EKS permissions
- SSH key pair created in your target region
- VPC with public subnets (default VPC works)

**Kernel Requirement:**
scx-slo requires **Linux 6.12+** with `CONFIG_SCHED_CLASS_EXT=y`. Standard AWS AMIs don't have this yet, so we'll use Fedora 41+ which ships with 6.12+.

---

## Part 1: Single EC2 Instance

Quick setup for development and basic testing.

### Step 1: Find Fedora 41 AMI

```bash
# Get latest Fedora 41 AMI (x86_64)
aws ec2 describe-images \
  --owners 125523088429 \
  --filters "Name=name,Values=Fedora-Cloud-Base-AmazonEC2.x86_64-41-*" \
  --query 'Images | sort_by(@, &CreationDate) | [-1].ImageId' \
  --output text \
  --region us-east-1

# For arm64 (Graviton instances):
aws ec2 describe-images \
  --owners 125523088429 \
  --filters "Name=name,Values=Fedora-Cloud-Base-AmazonEC2.aarch64-41-*" \
  --query 'Images | sort_by(@, &CreationDate) | [-1].ImageId' \
  --output text \
  --region us-east-1
```

### Step 2: Launch EC2 Instance

```bash
# Set variables
AMI_ID="ami-xxxxxxxxx"  # From step 1
KEY_NAME="your-key-pair"
INSTANCE_TYPE="t3.medium"  # Or t4g.medium for arm64

# Create security group
aws ec2 create-security-group \
  --group-name scx-slo-lab \
  --description "scx-slo lab instance"

SG_ID=$(aws ec2 describe-security-groups \
  --group-names scx-slo-lab \
  --query 'SecurityGroups[0].GroupId' --output text)

# Allow SSH
aws ec2 authorize-security-group-ingress \
  --group-id $SG_ID \
  --protocol tcp --port 22 --cidr 0.0.0.0/0

# Launch instance
aws ec2 run-instances \
  --image-id $AMI_ID \
  --instance-type $INSTANCE_TYPE \
  --key-name $KEY_NAME \
  --security-group-ids $SG_ID \
  --block-device-mappings '[{"DeviceName":"/dev/sda1","Ebs":{"VolumeSize":20}}]' \
  --tag-specifications 'ResourceType=instance,Tags=[{Key=Name,Value=scx-slo-lab}]' \
  --query 'Instances[0].InstanceId' --output text
```

### Step 3: Connect and Verify Kernel

```bash
# Get public IP
INSTANCE_ID="i-xxxxxxxxx"
PUBLIC_IP=$(aws ec2 describe-instances \
  --instance-ids $INSTANCE_ID \
  --query 'Reservations[0].Instances[0].PublicIpAddress' --output text)

# SSH in (Fedora uses 'fedora' user)
ssh -i ~/.ssh/$KEY_NAME.pem fedora@$PUBLIC_IP
```

On the instance:
```bash
# Verify kernel version (should be 6.12+)
uname -r

# Check sched_ext support
ls /sys/kernel/sched_ext/
cat /sys/kernel/sched_ext/state  # Should show "disabled" or "enabled"
```

### Step 4: Install Docker and Run scx-slo

```bash
# Install Docker
sudo dnf install -y docker
sudo systemctl enable --now docker
sudo usermod -aG docker fedora
newgrp docker

# Pull and run scx-slo
docker pull ghcr.io/srijanshukla18/scx-slo:latest

# Run the scheduler (needs privileges for BPF)
docker run -d --name scx-slo \
  --privileged \
  --pid=host \
  -v /sys/fs/bpf:/sys/fs/bpf \
  -v /sys/fs/cgroup:/sys/fs/cgroup \
  -v /sys/kernel/sched_ext:/sys/kernel/sched_ext \
  ghcr.io/srijanshukla18/scx-slo:latest -v

# Check logs
docker logs -f scx-slo
```

### Step 5: Verify Scheduler is Active

```bash
# Check sched_ext state
cat /sys/kernel/sched_ext/state
# Should show: enabled

# Check which scheduler is loaded
cat /sys/kernel/sched_ext/*/ops 2>/dev/null
# Should show: scx_slo

# Check metrics
curl localhost:8080/metrics | grep scx_slo
```

### Step 6: Demo - Noisy Neighbor Protection

Terminal 1 - Run latency-sensitive workload:
```bash
# Simple HTTP server
python3 -m http.server 8000 &

# Measure baseline latency
for i in {1..10}; do
  curl -w "%{time_total}\n" -o /dev/null -s http://localhost:8000/
done
```

Terminal 2 - Create CPU pressure:
```bash
# Install stress tool
sudo dnf install -y stress

# Saturate all CPUs
stress --cpu $(nproc) --timeout 60s
```

Terminal 1 - Measure latency under pressure:
```bash
# With scx-slo active, latency should remain stable
for i in {1..10}; do
  curl -w "%{time_total}\n" -o /dev/null -s http://localhost:8000/
done
```

### Cleanup EC2

```bash
# Stop scheduler
docker stop scx-slo

# Terminate instance
aws ec2 terminate-instances --instance-ids $INSTANCE_ID

# Delete security group (after instance terminates)
aws ec2 delete-security-group --group-id $SG_ID
```

---

## Part 2: EKS Cluster

Production-like deployment with Kubernetes.

### Challenge: Custom Node AMI

EKS nodes need Linux 6.12+ kernel. Options:
1. **Build custom AMI** with Fedora 41 configured for EKS (recommended)
2. **Use Bottlerocket** custom build (complex)
3. **Wait for Amazon Linux 2023** to get 6.12+ (not yet available)

### Step 1: Build Custom EKS Node AMI

We'll use Packer to build an EKS-compatible Fedora 41 AMI:

```bash
# Create packer template
cat > eks-fedora41.pkr.hcl << 'EOF'
packer {
  required_plugins {
    amazon = {
      version = ">= 1.0.0"
      source  = "github.com/hashicorp/amazon"
    }
  }
}

variable "region" {
  default = "us-east-1"
}

source "amazon-ebs" "fedora41-eks" {
  ami_name      = "eks-fedora41-{{timestamp}}"
  instance_type = "t3.medium"
  region        = var.region

  source_ami_filter {
    filters = {
      name                = "Fedora-Cloud-Base-AmazonEC2.x86_64-41-*"
      virtualization-type = "hvm"
    }
    owners      = ["125523088429"]
    most_recent = true
  }

  ssh_username = "fedora"
}

build {
  sources = ["source.amazon-ebs.fedora41-eks"]

  provisioner "shell" {
    inline = [
      "sudo dnf update -y",
      "sudo dnf install -y docker containerd kubelet kubectl",
      "sudo systemctl enable docker containerd kubelet",
      # EKS bootstrap requirements
      "curl -o /tmp/bootstrap.sh https://raw.githubusercontent.com/awslabs/amazon-eks-ami/master/templates/al2/runtime/bootstrap.sh",
      "chmod +x /tmp/bootstrap.sh"
    ]
  }
}
EOF

# Build AMI
packer build eks-fedora41.pkr.hcl
```

> **Note:** Building a fully EKS-compatible custom AMI is complex. For a simpler lab, use self-managed nodes or consider running a single-node k3s cluster on the EC2 instance from Part 1.

### Step 2: Create EKS Cluster (Self-Managed Nodes)

```bash
# Create cluster control plane only
eksctl create cluster \
  --name scx-slo-lab \
  --region us-east-1 \
  --version 1.31 \
  --without-nodegroup

# Add self-managed node group with custom AMI
eksctl create nodegroup \
  --cluster scx-slo-lab \
  --name sched-ext-nodes \
  --node-type t3.xlarge \
  --nodes 2 \
  --node-ami-family AmazonLinux2023 \
  --node-ami $CUSTOM_AMI_ID \
  --ssh-access \
  --ssh-public-key $KEY_NAME
```

### Alternative: k3s on EC2 (Simpler)

For a quicker lab, run k3s on the Fedora 41 EC2 instance:

```bash
# On the EC2 instance from Part 1
curl -sfL https://get.k3s.io | sh -

# Copy kubeconfig
mkdir -p ~/.kube
sudo cp /etc/rancher/k3s/k3s.yaml ~/.kube/config
sudo chown $(id -u):$(id -g) ~/.kube/config

# Verify
kubectl get nodes
```

### Step 3: Deploy scx-slo DaemonSet

First, update the DaemonSet image reference:

```bash
# Clone repo (if not already)
git clone https://github.com/srijanshukla18/scx-slo.git
cd scx-slo

# Update image in daemonset
sed -i 's|ghcr.io/yourorg/scx-slo-loader:v0.1.0|ghcr.io/srijanshukla18/scx-slo:latest|g' scx-slo-daemonset.yaml

# Deploy
kubectl apply -f scx-slo-daemonset.yaml

# Verify deployment
kubectl -n kube-system get pods -l app=scx-slo
kubectl -n kube-system logs -l app=scx-slo -c scheduler -f
```

### Step 4: Deploy Demo Workloads

```bash
# Deploy frontend (latency-sensitive) and batch (noisy neighbor)
kubectl apply -f demo-workloads.yaml

# Check pods are running
kubectl get pods

# Get frontend service URL
kubectl get svc frontend
```

### Step 5: Add SLO Annotations

```bash
# Patch frontend deployment with SLO annotations
kubectl patch deployment frontend -p '
{
  "spec": {
    "template": {
      "metadata": {
        "annotations": {
          "scx-slo/budget-ms": "20",
          "scx-slo/importance": "95"
        }
      }
    }
  }
}'

# Patch batch deployment with low priority
kubectl patch deployment batch -p '
{
  "spec": {
    "template": {
      "metadata": {
        "annotations": {
          "scx-slo/budget-ms": "500",
          "scx-slo/importance": "10"
        }
      }
    }
  }
}'

# Verify annotations were applied
kubectl get pods -o jsonpath='{range .items[*]}{.metadata.name}: {.metadata.annotations}{"\n"}{end}'
```

### Step 6: Generate Load and Observe

Terminal 1 - Watch scheduler metrics:
```bash
# Port-forward to scheduler metrics
kubectl -n kube-system port-forward ds/scx-slo-loader 8080:8080 &

# Watch metrics
watch -n 1 'curl -s localhost:8080/metrics | grep scx_slo'
```

Terminal 2 - Generate traffic to frontend:
```bash
# Get frontend URL
FRONTEND_URL=$(kubectl get svc frontend -o jsonpath='{.status.loadBalancer.ingress[0].hostname}')

# Generate load with latency measurement
while true; do
  curl -w "latency: %{time_total}s\n" -o /dev/null -s http://$FRONTEND_URL/get
  sleep 0.1
done
```

Terminal 3 - Scale up the noisy neighbor:
```bash
# Increase batch replicas to saturate nodes
kubectl scale deployment batch --replicas=4

# Watch frontend latency in Terminal 2
# With scx-slo, frontend latency should remain stable
# Without it, frontend would see p99 spikes
```

### Step 7: View Scheduler Logs

```bash
# Scheduler container logs (BPF loader)
kubectl -n kube-system logs -l app=scx-slo -c scheduler --tail=50

# Watcher container logs (K8s integration)
kubectl -n kube-system logs -l app=scx-slo -c watcher --tail=50

# Check for deadline misses
kubectl -n kube-system logs -l app=scx-slo -c scheduler | grep "DEADLINE MISS"
```

### Cleanup EKS

```bash
# Delete workloads
kubectl delete -f demo-workloads.yaml
kubectl delete -f scx-slo-daemonset.yaml

# Delete cluster
eksctl delete cluster --name scx-slo-lab

# Delete custom AMI (if created)
aws ec2 deregister-image --image-id $CUSTOM_AMI_ID
```

---

## Troubleshooting

### Scheduler won't start

```bash
# Check kernel version
uname -r  # Must be 6.12+

# Check sched_ext is available
ls /sys/kernel/sched_ext/state

# Check for conflicting scheduler
cat /sys/kernel/sched_ext/*/ops
```

### Init container fails kernel check

```bash
# Check init container logs
kubectl -n kube-system logs <pod-name> -c kernel-check

# Common issue: node AMI doesn't have 6.12+ kernel
```

### Watcher can't see pods

```bash
# Check RBAC
kubectl auth can-i list pods --as=system:serviceaccount:kube-system:scx-slo

# Check watcher logs for API errors
kubectl -n kube-system logs -l app=scx-slo -c watcher
```

### BPF map errors

```bash
# Check BPF filesystem is mounted
mount | grep bpf

# Check permissions on /sys/fs/bpf
ls -la /sys/fs/bpf/

# Look for pinned maps
ls /sys/fs/bpf/scx_slo/
```

---

## Cost Estimates

| Resource | Type | Hourly Cost | Notes |
|----------|------|-------------|-------|
| EC2 Instance | t3.medium | ~$0.04 | Single instance lab |
| EC2 Instance | t3.xlarge | ~$0.17 | For heavier testing |
| EKS Control Plane | - | ~$0.10 | Per cluster |
| EKS Nodes (2x) | t3.xlarge | ~$0.34 | 2-node cluster |

**Estimated lab cost:** $1-5 for a few hours of testing.

---

## Next Steps

1. **Production Hardening**: See `docs/security.md` for least-privilege deployment
2. **Monitoring**: Configure Prometheus to scrape `/metrics` endpoint
3. **Custom SLOs**: Edit ConfigMap to define workload-specific budgets
4. **Algorithm Details**: See `docs/deadline_algorithm.md` for EDF internals
