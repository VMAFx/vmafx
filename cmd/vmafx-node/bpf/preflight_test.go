// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

//go:build linux

// preflight_test.go — the host checks, the prefix validation, and the
// embedded object's agreement with the Go mirrors of its structs. None of
// these needs kernel privileges.

package bpf

import (
	"encoding/binary"
	"errors"
	"log/slog"
	"strings"
	"testing"

	"github.com/cilium/ebpf"
	"github.com/cilium/ebpf/btf"
)

func probe(release, capEffHex string, btfErr error) hostProbe {
	return hostProbe{
		osRelease: func() ([]byte, error) { return []byte(release + "\n"), nil },
		status: func() ([]byte, error) {
			return []byte("Name:\tvmafx-node\nCapInh:\t0000000000000000\nCapEff:\t" + capEffHex + "\n"), nil
		},
		btf:     func() error { return btfErr },
		tracefs: func() error { return nil },
	}
}

const (
	capsNone     = "0000000000000000"
	capsBPFPerf  = "000000c000000000" // bits 38, 39
	capsBPFOnly  = "0000008000000000" // bit 39
	capsSysAdmin = "0000000000200000" // bit 21
)

// TestPreflight_Accepts: a 5.15 or newer kernel with BTF and either
// capability set passes (positive, boundary at 5.15).
func TestPreflight_Accepts(t *testing.T) {
	t.Parallel()
	for _, p := range []hostProbe{
		probe("5.15.0", capsBPFPerf, nil),
		probe("7.2.8-2-cachyos", capsSysAdmin, nil),
		probe("6.8.0-45-generic", capsBPFPerf, nil),
	} {
		if err := p.check(); err != nil {
			t.Errorf("check: %v", err)
		}
	}
}

// TestPreflight_Refuses names every missing requirement (negative).
func TestPreflight_Refuses(t *testing.T) {
	t.Parallel()
	cases := map[string]hostProbe{
		"older than 5.15":                      probe("5.14.21", capsBPFPerf, nil),
		"cannot parse kernel release":          probe("linux", capsBPFPerf, nil),
		"kernel BTF":                           probe("6.1.0", capsBPFPerf, errors.New("no such file")),
		"lacks CAP_BPF and CAP_PERFMON":        probe("6.1.0", capsBPFOnly, nil),
		"lacks CAP_BPF and CAP_PERFMON (or CA": probe("6.1.0", capsNone, nil),
	}
	for want, p := range cases {
		if err := p.check(); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("check = %v, want it to mention %q", err, want)
		}
	}
	noTrace := probe("6.1.0", capsBPFPerf, nil)
	noTrace.tracefs = func() error { return errors.New("permission denied") }
	if err := noTrace.check(); err == nil || !strings.Contains(err.Error(), "tracefs") {
		t.Errorf("check = %v, want the tracefs refusal", err)
	}
	all := probe("4.19.0", capsNone, errors.New("absent"))
	all.tracefs = noTrace.tracefs
	for _, want := range []string{"older than", "BTF", "tracefs", "CAP_BPF"} {
		if err := all.check(); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("combined failure %v does not list %q", err, want)
		}
	}
}

// TestCapEff_Missing: a status without CapEff is an error, not "no caps".
func TestCapEff_Missing(t *testing.T) {
	t.Parallel()
	if _, err := capEff([]byte("Name:\tx\n")); err == nil {
		t.Fatal("capEff accepted a status without CapEff")
	}
}

// TestValidatePrefix: absolute and at most 255 bytes (boundary).
func TestValidatePrefix(t *testing.T) {
	t.Parallel()
	if err := validatePrefix("/" + strings.Repeat("a", 254)); err != nil {
		t.Errorf("255-byte prefix refused: %v", err)
	}
	if err := validatePrefix("/" + strings.Repeat("a", 255)); err == nil {
		t.Error("256-byte prefix accepted (it used to be truncated silently)")
	}
	if err := validatePrefix("rclone-mount/"); err == nil {
		t.Error("relative prefix accepted")
	}
}

// TestStart_StopsOnPreflight: Start returns the preflight failure before any
// BPF syscall (no privileges needed to observe it).
func TestStart_StopsOnPreflight(t *testing.T) {
	old := preflight
	t.Cleanup(func() { preflight = old })
	preflight = func() error { return errors.New("ebpf: process lacks CAP_BPF") }
	err := New("/mnt/x", slog.Default()).Start(t.Context())
	if err == nil || !strings.Contains(err.Error(), "lacks CAP_BPF") {
		t.Fatalf("Start = %v, want the preflight failure", err)
	}
}

// TestEmbeddedObjectMatchesMirrors: the bpf2go object carries the three
// tracepoint programs and four maps, and the value layouts match the Go
// mirrors the loader writes and reads.
func TestEmbeddedObjectMatchesMirrors(t *testing.T) {
	t.Parallel()
	requireGeneratedObject(t)
	spec, err := loadRcloneBypass()
	if err != nil {
		t.Fatalf("loadRcloneBypass: %v", err)
	}
	for _, name := range []string{"tp_openat_enter", "tp_openat_exit", "tp_close_enter"} {
		if p := spec.Programs[name]; p == nil || p.Type != ebpf.TracePoint {
			t.Errorf("program %s missing or not a tracepoint: %+v", name, p)
		}
	}
	checks := map[string]uint32{
		"mount_prefix_map": uint32(binary.Size(rcloneBypassMountPrefixT{})),
		"bypass_fds":       uint32(binary.Size(rcloneBypassBypassEntryT{})),
	}
	for name, want := range checks {
		if m := spec.Maps[name]; m == nil || m.ValueSize != want {
			t.Errorf("map %s value size = %+v, want %d", name, m, want)
		}
	}
	if m := spec.Maps["events"]; m == nil || m.Type != ebpf.RingBuf {
		t.Errorf("events map is %+v, want a ring buffer", m)
	}
	var event *btf.Struct
	if err := spec.Types.TypeByName("event_t", &event); err != nil {
		t.Fatalf("event_t not in the object's BTF: %v", err)
	}
	if got, want := event.Size, uint32(binary.Size(rcloneBypassEventT{})); got != want {
		t.Errorf("event_t is %d bytes in BPF, its Go mirror %d", got, want)
	}
}

// kernelLicence is the licence string the object declares to the kernel
// (ADR-1559): the source stays EUPL-1.2 and the loaded program is GPL under
// EUPL-1.2's compatibility clause. A string naming a licence the project never
// granted ("Dual BSD/GPL" before ADR-1559) fails, and so does one the kernel
// does not accept as GPL-compatible: the program calls GPL-only helpers
// (bpf_probe_read_user_str, bpf_probe_read_kernel) and would not load.
const kernelLicence = "GPL"

// TestEmbeddedObjectLicence: every program of the embedded object declares
// kernelLicence.
func TestEmbeddedObjectLicence(t *testing.T) {
	t.Parallel()
	requireGeneratedObject(t)
	spec, err := loadRcloneBypass()
	if err != nil {
		t.Fatalf("loadRcloneBypass: %v", err)
	}
	if len(spec.Programs) == 0 {
		t.Fatal("the embedded object has no programs")
	}
	for name, p := range spec.Programs {
		if p.License != kernelLicence {
			t.Errorf("program %s declares licence %q to the kernel, want %q", name, p.License, kernelLicence)
		}
	}
}

// TestEmbeddedObjectLoadsIntoKernel: on a host that passes Preflight (kernel
// 5.15+, BTF, tracefs, CAP_BPF and CAP_PERFMON or CAP_SYS_ADMIN) the kernel's
// verifier accepts the programs, GPL-only helper calls included, and the
// tracepoints attach. Elsewhere it skips and names the missing precondition.
func TestEmbeddedObjectLoadsIntoKernel(t *testing.T) {
	requireGeneratedObject(t)
	if err := Preflight(); err != nil {
		t.Skipf("this host cannot load the tracker: %v", err)
	}
	l := New(DefaultMountPrefix, slog.Default())
	if err := l.Start(t.Context()); err != nil {
		t.Fatalf("Start: %v", err)
	}
	l.Stop()
}

// TestDecodeEvent: a full record decodes, a truncated one is dropped, and a
// path without a NUL stops at maxPathLen (boundary).
func TestDecodeEvent(t *testing.T) {
	t.Parallel()
	size := binary.Size(rcloneBypassEventT{})
	raw := make([]byte, size)
	binary.NativeEndian.PutUint64(raw[8:], 42<<32|7)
	copy(raw[24:], "/rclone-mount/job/ref.y4m")
	key, path, ok := decodeEvent(raw)
	if !ok || key != 42<<32|7 || path != "/rclone-mount/job/ref.y4m" {
		t.Fatalf("decodeEvent = %d %q %v", key, path, ok)
	}
	if _, _, ok := decodeEvent(raw[:size-1]); ok {
		t.Fatal("a truncated record decoded")
	}
	for i := 24; i < size; i++ {
		raw[i] = 'a'
	}
	if _, path, ok := decodeEvent(raw); !ok || len(path) != maxPathLen {
		t.Fatalf("unterminated path: %d bytes, ok=%v; want %d", len(path), ok, maxPathLen)
	}
}
