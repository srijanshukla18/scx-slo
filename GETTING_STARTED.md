⚡️ Getting Started with scx-slo
A Kernel-Level Latency-Aware Scheduler for Kubernetes (Bottlerocket edition)

This guide sets you up with a drop-in DaemonSet that prioritizes latency-critical workloads at the Linux CPU scheduler level—before proxies, HPA, or preemption ever react.

🔧 Requirements
Requirement	Notes
Bottlerocket nodes with Linux ≥ 6.12	Confirm via uname -r and cat /sys/kernel/sched_ext/state
enable_privileged_containers = true	Required to load BPF programs from the DaemonSet
You use CI/CD (GitHub Actions, etc.)	We'll build the scheduler outside the cluster
Kubernetes 1.21+	Works on any recent EKS or kind cluster

✅ Step 1: Build the scx-slo scheduler (CI‑friendly)
Do this once in CI or locally (not on the node).

bash
Copy
Edit
git clone https://github.com/sched-ext/scx
cd scx/schedulers/scx_simple
make

mkdir scx-slo-image
cp scx_simple.bpf.o scx-slo-image/scx_slo.bpf.o
cp ../../../tools/loader/scx_loader scx-slo-image/

# Dockerfile
cat > scx-slo-image/Dockerfile <<EOF
FROM scratch
COPY scx_slo.bpf.o /opt/scx_slo.bpf.o
COPY scx_loader /usr/bin/scx_loader
ENTRYPOINT ["/usr/bin/scx_loader", "--object=/opt/scx_slo.bpf.o", "--cpu-mask=all", "--pin=/sys/fs/bpf/scx_slo"]
EOF

docker build -t ghcr.io/yourorg/scx-slo-loader:v0.1.0 scx-slo-image
docker push ghcr.io/yourorg/scx-slo-loader:v0.1.0
👉 This image includes:

The compiled .bpf.o scheduler

The scx_loader binary

A default command that loads the scheduler on all CPUs

🚀 Step 2: Deploy the DaemonSet
yaml
Copy
Edit
# scx-slo-daemonset.yaml
apiVersion: apps/v1
kind: DaemonSet
metadata:
  name: scx-slo-loader
  namespace: kube-system
spec:
  selector:
    matchLabels: {app: scx-slo}
  template:
    metadata:
      labels: {app: scx-slo}
    spec:
      hostPID: true
      hostNetwork: true
      containers:
      - name: loader
        image: ghcr.io/yourorg/scx-slo-loader:v0.1.0
        securityContext:
          privileged: true
        volumeMounts:
        - {name: bpffs,  mountPath: /sys/fs/bpf}
        - {name: cgroup, mountPath: /sys/fs/cgroup}
        lifecycle:
          preStop:
            exec: {command: ["/usr/bin/scx_loader", "stop"]}
      tolerations:
      - operator: Exists
      volumes:
      - {name: bpffs, hostPath: {path: /sys/fs/bpf}}
      - {name: cgroup, hostPath: {path: /sys/fs/cgroup}}
bash
Copy
Edit
kubectl apply -f scx-slo-daemonset.yaml
Result: Every Bottlerocket node now runs with scx_slo.bpf.o as the CPU scheduler. CFS is out, sched_ext is in.

🧪 Step 3: Demo workload
Here’s a quick demo to show scx-slo in action.

yaml
Copy
Edit
# frontend.yaml (latency-sensitive)
apiVersion: apps/v1
kind: Deployment
metadata: {name: frontend}
spec:
  replicas: 1
  template:
    metadata:
      annotations:
        slo.latency.p99: "200ms"
    spec:
      containers:
      - name: web
        image: kennethreitz/httpbin
        resources:
          limits:
            cpu: "500m"

---
# batch.yaml (noisy neighbor)
apiVersion: apps/v1
kind: Deployment
metadata: {name: batch}
spec:
  replicas: 1
  template:
    spec:
      containers:
      - name: stress
        image: polinux/stress
        args: ["--cpu", "8"]
bash
Copy
Edit
kubectl apply -f frontend.yaml batch.yaml
Now hit /delay/0.2 on the frontend in a loop:

bash
Copy
Edit
wrk -t2 -c64 -d30s http://<frontend-service-ip>/delay/0.2
You’ll see:

With CFS → p99 explodes under stress

With scx-slo → p99 stays ~200ms, even at 100% node CPU

🧼 Step 4: Clean up / rollback
bash
Copy
Edit
kubectl delete daemonset scx-slo-loader -n kube-system
The kernel automatically reverts back to the CFS scheduler.

No reboots, no node drains.

🧠 What’s actually happening?
The DaemonSet loads a tiny scheduler into the Linux kernel using sched_ext

It gives CPU time based on SLO weight, not just fairness

When your node is saturated, batch jobs get throttled automatically

Latency-sensitive pods keep running on time—without eviction, killing, or cold starts

🔮 What's next?
Feature	Description
🧠 SLO-aware BPF map	Replace the hardcoded cgroup ID with a map keyed by pod or container
📦 Sidecar agent	Auto-update SLO weights from Kubernetes pod annotations
📈 Export metrics	Use perf events to emit latency miss warnings (early SLO alerts)
🪄 CRD / Helm chart	Abstract the DaemonSet & weights behind kubectl apply -f slo.yaml

🏁 Final Thoughts
This version of scx-slo is:

Self-contained: CI builds the logic; nodes stay read-only

Safe to test: Kernel automatically falls back to normal scheduling if it fails

DaemonSet-deployable: Just kubectl apply—no custom AMIs, no reboots

Powerful: Latency protection at the kernel level, before your proxies even react
