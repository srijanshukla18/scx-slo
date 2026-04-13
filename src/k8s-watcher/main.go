package main

import (
	"context"
	"encoding/binary"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/cilium/ebpf"
	"golang.org/x/sys/unix"
	corev1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/fields"
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
	ctx := context.Background()
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
	selector := fields.OneTermEqualSelector("spec.nodeName", nodeName).String()

	// 3a. Reconcile existing pods to avoid missing state after restart
	podList, err := clientset.CoreV1().Pods("").List(ctx, metav1.ListOptions{
		FieldSelector: selector,
	})
	if err != nil {
		log.Fatalf("Failed to list pods: %v", err)
	}
	for _, pod := range podList.Items {
		if err := reconcilePod(&pod, m); err != nil {
			log.Printf("Reconcile failed for pod %s/%s: %v", pod.Namespace, pod.Name, err)
		}
	}

	// 3b. Watch for changes
	watch, err := clientset.CoreV1().Pods("").Watch(ctx, metav1.ListOptions{
		FieldSelector: selector,
	})
	if err != nil {
		log.Fatalf("Failed to watch pods: %v", err)
	}

	for event := range watch.ResultChan() {
		pod, ok := event.Object.(*corev1.Pod)
		if !ok {
			continue
		}

		switch event.Type {
		case "ADDED", "MODIFIED":
			if err := reconcilePod(pod, m); err != nil {
				log.Printf("Reconcile failed for pod %s/%s: %v", pod.Namespace, pod.Name, err)
			}
		case "DELETED":
			if err := removePodSLO(pod, m); err != nil {
				log.Printf("Cleanup failed for pod %s/%s: %v", pod.Namespace, pod.Name, err)
			}
		default:
			// ignore bookmarks / errors here
		}
	}
}

func reconcilePod(pod *corev1.Pod, m *ebpf.Map) error {
	budgetStr, hasBudget := pod.Annotations[AnnotationBudget]
	importStr, hasImportance := pod.Annotations[AnnotationImportance]

	if !hasBudget && !hasImportance {
		// Remove any stale entry for this pod
		return removePodSLO(pod, m)
	}

	// Parse SLO values
	budgetMs, err := strconv.ParseUint(budgetStr, 10, 64)
	if err != nil && hasBudget {
		return fmt.Errorf("invalid budget annotation %q: %w", budgetStr, err)
	}
	importance, err := strconv.ParseUint(importStr, 10, 32)
	if err != nil && hasImportance {
		return fmt.Errorf("invalid importance annotation %q: %w", importStr, err)
	}

	if budgetMs == 0 {
		budgetMs = 100 // Default 100ms
	}
	if importance == 0 {
		importance = 50 // Default 50
	}

	cgID, err := resolvePodCgroupID(pod)
	if err != nil {
		return fmt.Errorf("resolve cgroup id: %w", err)
	}

	cfg := sloCfg{
		BudgetNs:   budgetMs * 1_000_000,
		Importance: uint32(importance),
		Flags:      0,
	}

	if err := m.Update(cgID, cfg, ebpf.UpdateAny); err != nil {
		return fmt.Errorf("map update: %w", err)
	}

	log.Printf("Updated SLO for pod %s/%s (cgID=%d): budget=%dms, importance=%d",
		pod.Namespace, pod.Name, cgID, budgetMs, importance)
	return nil
}

func removePodSLO(pod *corev1.Pod, m *ebpf.Map) error {
	cgID, err := resolvePodCgroupID(pod)
	if err != nil {
		// If we can't resolve, there's nothing to delete safely
		return err
	}
	if err := m.Delete(cgID); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("map delete: %w", err)
	}
	log.Printf("Removed SLO for pod %s/%s (cgID=%d)", pod.Namespace, pod.Name, cgID)
	return nil
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
	fh, _, err := unix.NameToHandleAt(unix.AT_FDCWD, fullPath, 0)
	if err != nil {
		return 0, fmt.Errorf("failed to get handle for %s: %v", fullPath, err)
	}

	// The first 8 bytes of the handle's data for cgroupv2 is the 64-bit ID
	handleBytes := fh.Bytes()
	if len(handleBytes) < 8 {
		return 0, fmt.Errorf("handle too small for ID: %d", len(handleBytes))
	}

	cgID := binary.LittleEndian.Uint64(handleBytes[:8])
	return cgID, nil
}
