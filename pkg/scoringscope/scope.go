// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// pkg/scoringscope/scope.go — which scoring inputs a tenant may name
// (ADR-1577).
//
// A tenant's scoring roots are local directories and remote prefixes. An
// input is admitted only when it lies under one of them; a tenant without
// roots may score nothing (deny by default). The controller checks every
// path a caller names (Score, POST /v1/score, SubmitJob) and hands the roots
// to the node with each job; the node checks again where the files are.
//
// Inputs and roots are compared in one canonical form per kind:
//
//   - local: an absolute path ("/data/acme/ref.y4m", "file:///data/...").
//     A ".." element is refused outright, then the cleaned path must lie
//     under the cleaned root. Resolve also follows symlinks: the input's real
//     path must lie under the root's real path, and the real path is what the
//     caller scores, so a link swapped after the check is never followed.
//   - http(s): scheme and host must match (case-insensitive), the decoded
//     path must lie under the root's path; ".." (also percent-encoded) is
//     refused.
//   - rclone remote ("s3:bucket/acme/x", "s3://bucket/acme/x",
//     "rclone://remote:path", ":local:/data"): the remote name must match and
//     the path must lie under the root's path; ".." is refused.
//
// A relative local path is refused: it would depend on the reader's working
// directory.

// Package scoringscope decides whether a scoring input lies under a tenant's
// scoring roots.
package scoringscope

import (
	"errors"
	"fmt"
	"net/url"
	"path"
	"path/filepath"
	"strings"

	"github.com/VMAFx/vmafx/pkg/storage"
)

// MaxRoots bounds the roots of one tenant (the CRD's maxItems).
const MaxRoots = 32

// ErrOutside is wrapped by every refusal of an input.
var ErrOutside = errors.New("outside the tenant's scoring roots")

type kind int

const (
	kindLocal kind = iota
	kindHTTP
	kindRemote
)

// ref is one input or root in canonical form: for kindLocal, base is empty
// and p is the cleaned absolute path; for kindHTTP, base is "scheme://host";
// for kindRemote, base is the remote name with its colon ("s3:", ":local:").
type ref struct {
	kind kind
	base string
	p    string
}

// Roots is a validated, immutable list of scoring roots.
type Roots struct {
	raw  []string
	refs []ref
}

// Parse validates raw roots. Every entry must be an absolute local path, an
// http(s) URL with a host, or an rclone remote; ".." is refused.
func Parse(raw []string) (Roots, error) {
	if len(raw) > MaxRoots {
		return Roots{}, fmt.Errorf("scoring roots: %d entries, at most %d", len(raw), MaxRoots)
	}
	out := Roots{raw: make([]string, 0, len(raw)), refs: make([]ref, 0, len(raw))}
	for _, r := range raw {
		trimmed := strings.TrimSpace(r)
		c, err := canonical(trimmed)
		if err != nil {
			return Roots{}, fmt.Errorf("scoring root %q: %w", r, err)
		}
		out.raw = append(out.raw, trimmed)
		out.refs = append(out.refs, c)
	}
	return out, nil
}

// Strings returns the roots as configured (trimmed), for handing to a node.
func (r Roots) Strings() []string { return append([]string(nil), r.raw...) }

// Empty reports whether no root is configured (every input is refused).
func (r Roots) Empty() bool { return len(r.refs) == 0 }

// Check admits input when it lies under a root, comparing canonical forms
// only (no file system access). It returns an error wrapping ErrOutside
// otherwise.
func (r Roots) Check(input string) error {
	_, err := r.match(input)
	return err
}

// Resolve admits input like Check and, for a local input, follows its
// symlinks: the real path must lie under the real path of the root that
// admitted it. It returns what the caller should read: the real path of a
// local input, the input unchanged otherwise.
func (r Roots) Resolve(input string) (string, error) {
	in, err := r.match(input)
	if err != nil {
		return "", err
	}
	if in.kind != kindLocal {
		return input, nil
	}
	resolved, err := filepath.EvalSymlinks(in.p)
	if err != nil {
		return "", fmt.Errorf("scoring input %q: resolve: %w", input, err)
	}
	for _, root := range r.refs {
		if root.kind != kindLocal {
			continue
		}
		rootReal, rerr := filepath.EvalSymlinks(root.p)
		if rerr == nil && under(resolved, rootReal) {
			return resolved, nil
		}
	}
	return "", fmt.Errorf("scoring input %q resolves to %q, %w", input, resolved, ErrOutside)
}

// match canonicalises input and returns it when a root admits it.
func (r Roots) match(input string) (ref, error) {
	if r.Empty() {
		return ref{}, fmt.Errorf("scoring input %q: the tenant has no scoring roots, %w", input, ErrOutside)
	}
	in, err := canonical(strings.TrimSpace(input))
	if err != nil {
		return ref{}, fmt.Errorf("scoring input %q: %v, %w", input, err, ErrOutside)
	}
	for _, root := range r.refs {
		if root.kind == in.kind && root.base == in.base && under(in.p, root.p) {
			return in, nil
		}
	}
	return ref{}, fmt.Errorf("scoring input %q is %w", input, ErrOutside)
}

// canonical classifies s and returns its canonical form.
func canonical(s string) (ref, error) {
	switch {
	case s == "":
		return ref{}, errors.New("empty")
	case storage.IsHTTP(s):
		return canonicalHTTP(s)
	case strings.HasPrefix(s, "/") || strings.HasPrefix(s, "file://"):
		return canonicalLocal(s)
	case storage.IsLocal(s) && !strings.Contains(s, ":"):
		return ref{}, errors.New("a relative path is not accepted; give an absolute path")
	default:
		return canonicalRemote(s)
	}
}

func canonicalLocal(s string) (ref, error) {
	p := s
	if strings.HasPrefix(s, "file://") {
		u, err := url.Parse(s)
		if err != nil || u.Host != "" {
			return ref{}, errors.New("not a file:///absolute/path URL")
		}
		p = u.Path
	}
	if !strings.HasPrefix(p, "/") {
		return ref{}, errors.New("a relative path is not accepted; give an absolute path")
	}
	if hasDotDot(p) {
		return ref{}, errors.New(`".." is not accepted`)
	}
	return ref{kind: kindLocal, p: path.Clean(p)}, nil
}

func canonicalHTTP(s string) (ref, error) {
	u, err := url.Parse(s)
	if err != nil {
		return ref{}, fmt.Errorf("not a URL: %w", err)
	}
	if hasDotDot(u.Path) {
		return ref{}, errors.New(`".." is not accepted`)
	}
	return ref{kind: kindHTTP, base: strings.ToLower(u.Scheme + "://" + u.Host), p: path.Clean("/" + u.Path)}, nil
}

func canonicalRemote(s string) (ref, error) {
	rp, err := storage.RcloneRemote(s)
	if err != nil {
		return ref{}, err
	}
	name, p, ok := splitRemote(rp)
	if !ok {
		return ref{}, errors.New("not an absolute path, an http(s) URL or an rclone remote (remote:path)")
	}
	if hasDotDot(p) {
		return ref{}, errors.New(`".." is not accepted`)
	}
	return ref{kind: kindRemote, base: name, p: path.Clean("/" + p)}, nil
}

// splitRemote splits "name:path" or ":backend:path" into the remote with its
// colon(s) and the path.
func splitRemote(rp string) (string, string, bool) {
	start := 0
	if strings.HasPrefix(rp, ":") {
		start = 1
	}
	i := strings.Index(rp[start:], ":")
	if i <= 0 {
		return "", "", false
	}
	cut := start + i + 1
	return rp[:cut], rp[cut:], true
}

// hasDotDot reports whether a slash-separated path has a ".." element.
func hasDotDot(p string) bool {
	for el := range strings.SplitSeq(p, "/") {
		if el == ".." {
			return true
		}
	}
	return false
}

// under reports whether the cleaned path p is root or lies inside it.
func under(p, root string) bool {
	if root == "/" || p == root {
		return true
	}
	return strings.HasPrefix(p, root+"/")
}
