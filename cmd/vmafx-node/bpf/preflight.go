// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

//go:build linux

// preflight.go — host checks run before the loader makes any BPF syscall, so a
// node that cannot run the tracker stops with the reason instead of a raw
// EPERM or verifier error (ADR-1539).

package bpf

import (
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"
)

const (
	minKernelMajor = 5
	minKernelMinor = 15

	capSysAdmin = 21
	capPerfmon  = 38
	capBPF      = 39
)

// hostProbe reads the host facts Preflight checks; tests replace the readers.
type hostProbe struct {
	osRelease func() ([]byte, error) // /proc/sys/kernel/osrelease
	status    func() ([]byte, error) // /proc/self/status
	btf       func() error           // stat /sys/kernel/btf/vmlinux
	tracefs   func() error           // stat the sys_enter_openat tracepoint id
}

var systemProbe = hostProbe{
	osRelease: func() ([]byte, error) { return os.ReadFile("/proc/sys/kernel/osrelease") },
	status:    func() ([]byte, error) { return os.ReadFile("/proc/self/status") },
	btf: func() error {
		_, err := os.Stat("/sys/kernel/btf/vmlinux")
		return err
	},
	tracefs: statTracepointID,
}

// statTracepointID finds the openat tracepoint's id file in tracefs, where
// cilium/ebpf's link.Tracepoint looks it up (tracefs, then debugfs).
func statTracepointID() error {
	var errs []error
	for _, root := range []string{"/sys/kernel/tracing", "/sys/kernel/debug/tracing"} {
		_, err := os.Stat(root + "/events/syscalls/sys_enter_openat/id")
		if err == nil {
			return nil
		}
		errs = append(errs, err)
	}
	return errors.Join(errs...)
}

// preflight is what Start runs; a variable so tests can bypass the host.
var preflight = Preflight

// SetPreflightForTest replaces the host check Start runs and returns a
// function that restores it. Test-only (the ForTest suffix is the contract);
// production code never calls it.
func SetPreflightForTest(fn func() error) (restore func()) {
	old := preflight
	preflight = fn
	return func() { preflight = old }
}

// Preflight reports every reason this host cannot run the tracker: a kernel
// older than 5.15, no kernel BTF, no visible syscall tracepoints, or a process
// without CAP_BPF and CAP_PERFMON (or CAP_SYS_ADMIN). It returns nil when all
// hold.
func Preflight() error { return systemProbe.check() }

func (p hostProbe) check() error {
	return errors.Join(p.checkKernel(), p.checkBTF(), p.checkTracefs(), p.checkCapabilities())
}

func (p hostProbe) checkTracefs() error {
	if err := p.tracefs(); err != nil {
		return fmt.Errorf("ebpf: syscall tracepoints not visible in tracefs (mount /sys/kernel/tracing): %w", err)
	}
	return nil
}

func (p hostProbe) checkKernel() error {
	raw, err := p.osRelease()
	if err != nil {
		return fmt.Errorf("ebpf: read kernel release: %w", err)
	}
	release := strings.TrimSpace(string(raw))
	major, minor, ok := parseRelease(release)
	if !ok {
		return fmt.Errorf("ebpf: cannot parse kernel release %q", release)
	}
	if major < minKernelMajor || (major == minKernelMajor && minor < minKernelMinor) {
		return fmt.Errorf("ebpf: kernel %s is older than %d.%d (ring buffer, BTF and bounded loops)", release, minKernelMajor, minKernelMinor)
	}
	return nil
}

func (p hostProbe) checkBTF() error {
	if err := p.btf(); err != nil {
		return fmt.Errorf("ebpf: kernel BTF /sys/kernel/btf/vmlinux unavailable (mount /sys/kernel/btf into the container): %w", err)
	}
	return nil
}

func (p hostProbe) checkCapabilities() error {
	raw, err := p.status()
	if err != nil {
		return fmt.Errorf("ebpf: read process capabilities: %w", err)
	}
	effective, err := capEff(raw)
	if err != nil {
		return err
	}
	has := func(bit uint) bool { return effective&(1<<bit) != 0 }
	if has(capSysAdmin) || (has(capBPF) && has(capPerfmon)) {
		return nil
	}
	return fmt.Errorf("ebpf: process lacks CAP_BPF and CAP_PERFMON (or CAP_SYS_ADMIN); CapEff=%#x "+
		"(Kubernetes: securityContext.capabilities.add [BPF, PERFMON])", effective)
}

// parseRelease extracts major and minor from a release such as
// "6.8.0-45-generic" or "7.2.8-2-cachyos".
func parseRelease(release string) (int, int, bool) {
	parts := strings.SplitN(release, ".", 3)
	if len(parts) < 2 {
		return 0, 0, false
	}
	major, err1 := strconv.Atoi(parts[0])
	minorDigits := strings.TrimRightFunc(parts[1], func(r rune) bool { return r < '0' || r > '9' })
	minor, err2 := strconv.Atoi(minorDigits)
	return major, minor, err1 == nil && err2 == nil
}

// capEff returns the effective capability set from /proc/self/status.
func capEff(status []byte) (uint64, error) {
	for line := range strings.SplitSeq(string(status), "\n") {
		if rest, ok := strings.CutPrefix(line, "CapEff:"); ok {
			v, err := strconv.ParseUint(strings.TrimSpace(rest), 16, 64)
			if err != nil {
				return 0, fmt.Errorf("ebpf: parse CapEff %q: %w", rest, err)
			}
			return v, nil
		}
	}
	return 0, errors.New("ebpf: /proc/self/status has no CapEff line")
}
