// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent OR MIT

// rclone_bypass.bpf.c — eBPF program for FUSE read-path bypass.
//
// Strategy: intercept vfs_read() via kprobe/fentry on the read(2) syscall
// for file descriptors whose path prefix is /rclone-mount/.  When active,
// the program records per-FD open-file metadata into a shared BPF map that
// the Go side uses to direct-read from the backing cache instead of going
// through the FUSE daemon.
//
// Kernel version requirement: Linux 5.15+ (BPF CO-RE, fentry, bpf_d_path).
// Capability requirement: CAP_SYS_ADMIN or CAP_BPF (Linux 5.8+).
//
// The program does NOT bypass data reads itself — it is a probe-only design.
// It marks FDs that are FUSE-backed under /rclone-mount/ in the bypass_fds
// map; the Go loader then uses the map to decide whether to use pread(2) on
// the backing file directly.  No kernel memory is modified; the eBPF program
// is read-only with respect to file system state.
//
// ADR-0779.

//go:build ignore
// +build ignore

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#define MAX_PATH_LEN 256
#define MAX_TRACKED_FDS 4096

// Mount prefix to intercept.  The Go loader writes the actual prefix into
// mount_prefix_map at index 0 before attaching the program.
struct mount_prefix_t {
    char prefix[MAX_PATH_LEN];
    __u32 prefix_len;
};

// Per-FD bypass entry stored in bypass_fds map.
struct bypass_entry_t {
    __u64 backing_inode; // inode number of the cached backing file
    __u32 flags;         // reserved for future flag bits
    __u8 active;         // 1 = this FD is eligible for bypass
};

// Map: index 0 → mount prefix config written by Go loader.
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct mount_prefix_t);
} mount_prefix_map SEC(".maps");

// Map: pid_tgid → path scratch buffer for open/read probes.
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);
    __type(value, char[MAX_PATH_LEN]);
} scratch_paths SEC(".maps");

// Map: global fd key (pid<<32|fd) → bypass_entry_t.
// Populated on openat/open for FUSE files; read by Go side.
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_TRACKED_FDS);
    __type(key, __u64);
    __type(value, struct bypass_entry_t);
} bypass_fds SEC(".maps");

// Map: ring buffer for events (new FD eligible for bypass).
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 17); // 128 KiB
} events SEC(".maps");

struct event_t {
    __u64 pid_tgid;
    __u64 fd_key; // pid<<32 | fd
    __u64 inode;
    char path[MAX_PATH_LEN];
};

// Probe: sys_enter_openat — capture path arg before the syscall.
SEC("tracepoint/syscalls/sys_enter_openat")
int tp_openat_enter(struct trace_event_raw_sys_enter *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    const char __user *user_path = (const char __user *)ctx->args[1];

    char path[MAX_PATH_LEN] = {};
    long ret = bpf_probe_read_user_str(path, sizeof(path), user_path);
    if (ret <= 0)
        return 0;

    // Only track paths that start with the configured mount prefix.
    __u32 zero = 0;
    struct mount_prefix_t *mp = bpf_map_lookup_elem(&mount_prefix_map, &zero);
    if (!mp || mp->prefix_len == 0)
        return 0;

    // Manual prefix match (verifier-safe: bounded loop).
    __u32 plen = mp->prefix_len;
    if (plen > MAX_PATH_LEN - 1)
        plen = MAX_PATH_LEN - 1;

    bool match = true;
    for (__u32 i = 0; i < MAX_PATH_LEN && i < plen; i++) {
        if (path[i] != mp->prefix[i]) {
            match = false;
            break;
        }
    }
    if (!match)
        return 0;

    bpf_map_update_elem(&scratch_paths, &pid_tgid, &path, BPF_ANY);
    return 0;
}

// Probe: sys_exit_openat — record the returned fd if path was matched.
SEC("tracepoint/syscalls/sys_exit_openat")
int tp_openat_exit(struct trace_event_raw_sys_exit *ctx)
{
    __u64 pid_tgid = bpf_get_current_pid_tgid();

    char *path = bpf_map_lookup_elem(&scratch_paths, &pid_tgid);
    if (!path) {
        return 0;
    }
    bpf_map_delete_elem(&scratch_paths, &pid_tgid);

    long fd = ctx->ret;
    if (fd < 0)
        return 0;

    __u32 pid = (__u32)(pid_tgid >> 32);
    __u64 fd_key = ((__u64)pid << 32) | (__u64)(unsigned int)fd;

    struct bypass_entry_t entry = {
        .backing_inode = 0,
        .flags = 0,
        .active = 1,
    };
    bpf_map_update_elem(&bypass_fds, &fd_key, &entry, BPF_ANY);

    // Emit ring-buffer event so Go side can immediately act.
    struct event_t *ev = bpf_ringbuf_reserve(&events, sizeof(*ev), 0);
    if (!ev)
        return 0;

    ev->pid_tgid = pid_tgid;
    ev->fd_key = fd_key;
    ev->inode = 0;
    // Copy up to MAX_PATH_LEN bytes — verifier-safe bpf_probe_read_kernel.
    bpf_probe_read_kernel(ev->path, sizeof(ev->path), path);
    bpf_ringbuf_submit(ev, 0);

    return 0;
}

// Probe: sys_enter_close — remove FD from bypass map when it is closed.
SEC("tracepoint/syscalls/sys_enter_close")
int tp_close_enter(struct trace_event_raw_sys_enter *ctx)
{
    __u32 pid = (__u32)(bpf_get_current_pid_tgid() >> 32);
    __u64 fd = (__u64)(unsigned int)ctx->args[0];
    __u64 fd_key = ((__u64)pid << 32) | fd;

    bpf_map_delete_elem(&bypass_fds, &fd_key);
    return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
