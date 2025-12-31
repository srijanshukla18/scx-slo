package main

import (
	"context"
	"fmt"
	"log"
	"os"
	"strconv"
	"time"

	"github.com/cilium/ebpf"
	corev1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/rest"
)

const (
	AnnotationBudget     = "scx-slo/budget-ms"
	AnnotationImportance = "scx-slo/importance"
	PinnedMapPath        = "/sys/fs/bpf/slo_map"
)

// Simplified slo_cfg struct to match BPF side
type sloCfg struct {
	BudgetNs   uint64
	Importance uint32
	Flags      uint32
}

func main() {
	nodeName := os.Getenv("NODE_NAME")
	if nodeName == "" {
		log.Fatal("NODE_NAME environment variable not set")
	}

	// 1. Connect to Kubernetes API
	config, err := rest.InClusterConfig()
	if err != nil {
		log.Fatalf("Failed to get in-cluster config: %v", err)
	}
	clientset, err := kubernetes.NewForConfig(config)
	if err != nil {
		log.Fatalf("Failed to create clientset: %v", err)
	}

	// 2. Open the pinned BPF map
	// Note: We use the cilium/ebpf library for easy map interaction
	m, err := ebpf.LoadPinnedMap(PinnedMapPath, nil)
	if err != nil {
		log.Fatalf("Failed to load pinned map at %s: %v", PinnedMapPath, err)
	}
	defer m.Close()

	log.Printf("Starting K8s watcher for node %s", nodeName)

	// 3. Watch pods on this node
	watch, err := clientset.CoreV1().Pods("").Watch(context.TODO(), metav1.ListOptions{
		FieldSelector: fmt.Sprintf("spec.nodeName=%s", nodeName),
	})
	if err != nil {
		log.Fatalf("Failed to watch pods: %v", err)
	}

	for event := range watch.ResultChan() {
		pod, ok := event.Object.(*corev1.Pod)
		if !ok {
			continue
		}

		budgetStr, hasBudget := pod.Annotations[AnnotationBudget]
		importStr, hasImportance := pod.Annotations[AnnotationImportance]

		if !hasBudget && !hasImportance {
			continue
		}

		// Parse SLO values
		budgetMs, _ := strconv.ParseUint(budgetStr, 10, 64)
		importance, _ := strconv.ParseUint(importStr, 10, 32)

		if budgetMs == 0 {
			budgetMs = 100 // Default 100ms
		}
		if importance == 0 {
			importance = 50 // Default 50
		}

		// Find Cgroup ID (Simplified: we use internal K8s logic or path resolution)
		// This is a placeholder for the actual Cgroup resolution logic
		// which usually involves reading /proc/<pid>/cgroup for one of the pod's containers
		cgID, err := resolvePodCgroupID(pod)
		if err != nil {
			log.Printf("Could not resolve Cgroup ID for pod %s: %v", pod.Name, err)
			continue
		}

		// Update BPF Map
		cfg := sloCfg{
			BudgetNs:   budgetMs * 1000000,
			Importance: uint32(importance),
			Flags:      0,
		}

		if err := m.Update(cgID, cfg, ebpf.UpdateAny); err != nil {
			log.Printf("Failed to update BPF map for pod %s (cgID %d): %v", pod.Name, cgID, err)
		} else {
			log.Printf("Updated SLO for pod %s: budget=%dms, importance=%d", pod.Name, budgetMs, importance)
		}
	}
}

// resolvePodCgroupID finds the 64-bit kernel cgroup ID for a given pod.
// It constructs the cgroup path based on Pod UID and QOS class, then
// uses name_to_handle_at to get the inode-based ID.
func resolvePodCgroupID(pod *corev1.Pod) (uint64, error) {
	uid := strings.ReplaceAll(string(pod.UID), "-", "_")
	qos := strings.ToLower(string(pod.Status.QOSClass))
	
	// Construct the path (Standard for cgroupv2/systemd)
	// Example: /sys/fs/cgroup/kubepods.slice/kubepods-burstable.slice/kubepods-burstable-pod<UID>.slice
	basePath := "/sys/fs/cgroup/kubepods.slice"
	qosPath := fmt.Sprintf("kubepods-%s.slice", qos)
	podPath := fmt.Sprintf("kubepods-%s-pod%s.slice", qos, uid)
	
	fullPath := filepath.Join(basePath, qosPath, podPath)
	
	// Check if path exists
	if _, err := os.Stat(fullPath); os.IsNotExist(err) {
		// Fallback for older K8s/different runtimes
		fullPath = filepath.Join(basePath, podPath)
	}

	// Use name_to_handle_at to get the file handle (contains cgroup ID)
	var mountID int32
	handle := make([]byte, 128)
	fh := (*unix.FileHandle)(unsafe.Pointer(&handle[0]))
	fh.Size = 128 - 8 // Reserve space for the header

	err := unix.NameToHandleAt(unix.AT_FDCWD, fullPath, fh, &mountID, 0)
	if err != nil {
		return 0, fmt.Errorf("failed to get handle for %s: %v", fullPath, err)
	}

	// The first 8 bytes of the handle's data for cgroupv2 is the 64-bit ID
	if fh.Size < 8 {
		return 0, fmt.Errorf("handle too small for ID: %d", fh.Size)
	}
	
	cgID := *(*uint64)(unsafe.Pointer(&handle[8]))
	return cgID, nil
}
