# Security Considerations and Threat Model

This document outlines the security considerations, threat model, and mitigations implemented in scx-slo.

## Executive Summary

The SLO scheduler operates at the kernel level with privileged access to system scheduling. This document identifies potential attack vectors and the security measures implemented to mitigate them.

## Threat Model

### Assets Protected
- **System Stability**: Preventing kernel crashes or hangs
- **Scheduling Fairness**: Preventing priority escalation attacks
- **Resource Isolation**: Maintaining cgroup boundaries
- **Performance**: Preventing DoS attacks that degrade system performance

### Trust Boundaries
1. **Kernel ↔ BPF Program**: BPF verifier provides memory safety
2. **BPF Program ↔ Userspace Agent**: Ring buffer and map interfaces
3. **Userspace Agent ↔ Configuration**: File system permissions
4. **Administrator ↔ System**: Standard Linux privilege model

### Threat Actors
- **Unprivileged Users**: Cannot modify SLO configurations
- **Privileged Users**: Can modify configs but are bounded by validation
- **Compromised Containers**: Limited to their cgroup context
- **Malicious Administrators**: Trusted but input is still validated

## Attack Vectors and Mitigations

### 1. DoS Attacks via Malicious SLO Configuration

#### Attack: Zero Budget Starvation
**Vector**: Setting budget_ns = 0 to gain infinite priority
```c
// Malicious config
struct slo_cfg malicious = {
    .budget_ns = 0,        // Infinite priority!
    .importance = 100,
    .flags = 0
};
```

**Impact**: Task gets highest priority, starving all other tasks

**Mitigation**: Input validation with hard bounds
```c
// Defense implemented
if (cfg->budget_ns == 0 || cfg->budget_ns < MIN_BUDGET_NS || cfg->budget_ns > MAX_BUDGET_NS)
    return DEFAULT_BUDGET_NS;
```

#### Attack: Overflow Budget
**Vector**: Setting budget_ns = UINT64_MAX to cause overflow
**Impact**: Arithmetic overflow in deadline calculation
**Mitigation**: Upper bound validation (MAX_BUDGET_NS = 10 seconds)

#### Attack: Ring Buffer Flooding
**Vector**: Triggering massive deadline miss events to exhaust memory
**Impact**: System memory exhaustion, performance degradation
**Mitigation**: Rate limiting (1000 events/second per CPU)

### 2. Privilege Escalation Attacks

#### Attack: Cgroup Manipulation
**Vector**: Unprivileged process modifying SLO maps for privileged cgroups
**Impact**: Lower-privilege workloads getting higher scheduling priority
**Mitigation**: 
- BPF map permissions (CAP_BPF required)
- Configuration file permissions (root-only access)
- Runtime privilege validation

#### Attack: Task Context Poisoning
**Vector**: Manipulating task contexts to gain scheduling advantage
**Impact**: Tasks getting incorrect deadlines or priorities
**Mitigation**: 
- Task context isolation (PID-based keys)
- Automatic cleanup on task exit
- Validation on all context operations

### 3. Resource Exhaustion Attacks

#### Attack: Map Exhaustion
**Vector**: Creating unlimited cgroups or tasks to exhaust BPF map space
**Impact**: Memory exhaustion, system instability
**Mitigation**: Fixed map size limits
```c
#define MAX_CGROUPS 10000   // Bounded cgroup entries
#define MAX_TASKS 100000    // Bounded task entries
```

#### Attack: Computation DoS
**Vector**: Triggering expensive operations in hot scheduling paths
**Impact**: System performance degradation
**Mitigation**: 
- Optimized BPF code with minimal complexity
- Cached lookups where possible
- Rate limiting on expensive operations

### 4. Information Disclosure

#### Attack: Timing Analysis
**Vector**: Using deadline miss patterns to infer system load or task behavior
**Impact**: Information leakage about other workloads
**Mitigation**: 
- Aggregated statistics only
- Rate limiting prevents fine-grained timing analysis
- No task-specific information in events

#### Attack: Configuration Exposure
**Vector**: Reading SLO configurations to understand system priorities
**Impact**: Information about business-critical vs background workloads
**Mitigation**: File permissions restrict config access to root

## Security Measures Implemented

### 1. Input Validation
All user-controlled inputs are strictly validated:

```c
// Budget validation
#define MIN_BUDGET_NS (1 * NSEC_PER_MSEC)    // 1ms minimum
#define MAX_BUDGET_NS (10 * NSEC_PER_SEC)    // 10s maximum

// Importance validation
#define MIN_IMPORTANCE 1
#define MAX_IMPORTANCE 100

// Validation function
static inline int validate_slo_cfg(struct slo_cfg *cfg) {
    if (cfg->budget_ns < MIN_BUDGET_NS || cfg->budget_ns > MAX_BUDGET_NS)
        return -1;
    if (cfg->importance < MIN_IMPORTANCE || cfg->importance > MAX_IMPORTANCE)
        return -1;
    return 0;
}
```

### 2. Rate Limiting
Prevents event flooding attacks:

```c
#define MAX_EVENTS_PER_SEC 1000
#define RATE_LIMIT_WINDOW_NS (1 * NSEC_PER_SEC)

static inline bool is_rate_limited(void) {
    // Per-CPU rate limiting implementation
    // Allows maximum 1000 events per second per CPU
}
```

### 3. Resource Bounds
Fixed limits prevent exhaustion:

```c
// Map size limits
struct {
    __uint(max_entries, MAX_CGROUPS);  // 10,000 cgroups max
} slo_map SEC(".maps");

struct {
    __uint(max_entries, MAX_TASKS);    // 100,000 tasks max
} task_ctx_map SEC(".maps");
```

### 4. Safe Arithmetic
Prevents overflow attacks:

```c
// Safe deadline calculation
u64 deadline = now + budget_ns;  // budget_ns is bounded, preventing overflow
```

### 5. Proper Cleanup
Prevents memory leaks:

```c
// Automatic cleanup on task exit
if (!runnable) {
    bpf_map_delete_elem(&task_ctx_map, &pid);
}
```

## Deployment Security

### 1. File Permissions
```bash
# Configuration directory
chmod 755 /etc/scx-slo/
chmod 644 /etc/scx-slo/config

# Binary permissions
chmod 755 /usr/local/bin/scx_slo
```

### 2. Capability Requirements
The scheduler requires these capabilities:
- `CAP_BPF`: Load BPF programs
- `CAP_SYS_ADMIN`: Attach to scheduler hooks

### 3. Container Security
When running in containers:
- Use security profiles (AppArmor/SELinux)
- Limit container capabilities
- Use read-only root filesystems where possible

## Monitoring and Alerting

### 1. Security Metrics
Monitor these for potential attacks:
- Deadline miss event rate spikes
- Configuration file modification attempts
- Unusual cgroup activity patterns
- BPF program load/unload events

### 2. Audit Logging
Enable audit logging for:
- SLO configuration changes
- Scheduler attachment/detachment
- Privilege escalation attempts

## Incident Response

### 1. Suspected Attack Response
1. **Immediate**: Detach scheduler (`pkill scx_slo`)
2. **Assess**: Check logs for attack indicators
3. **Mitigate**: Review and fix configuration
4. **Recover**: Restart with validated configuration

### 2. Emergency Procedures
- Kernel automatically falls back to CFS on BPF program failure
- No persistent system changes (scheduler is hot-swappable)
- Configuration can be reset to defaults quickly

## Security Testing

### 1. Fuzzing
- Configuration file parsing
- BPF map updates
- Ring buffer operations

### 2. Stress Testing
- Resource exhaustion scenarios
- Malicious configuration injection
- High-load deadline miss storms

### 3. Penetration Testing
- Privilege escalation attempts
- Container escape scenarios
- Information disclosure tests

## Compliance Considerations

### 1. Standards Alignment
- Common Criteria security principles
- NIST Cybersecurity Framework
- CIS Controls for Linux systems

### 2. Regulatory Requirements
Consider these for regulated environments:
- Audit trail requirements
- Access control standards
- Security monitoring mandates

## Future Security Enhancements

### 1. Enhanced Authentication
- Digital signatures for configuration
- Hardware security module integration
- Certificate-based access control

### 2. Advanced Monitoring
- Machine learning anomaly detection
- Real-time security dashboards
- Integration with SIEM systems

### 3. Formal Verification
- Mathematical proof of security properties
- Model checking for race conditions
- Static analysis integration

## Conclusion

The scx-slo scheduler implements defense-in-depth security with multiple layers of protection against common attack vectors. Regular security reviews and updates ensure continued protection against evolving threats.