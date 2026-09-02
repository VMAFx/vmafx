// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.
//
// impl_gosec_test.go — regression tests for the gosec-G304 hardening of
// describeModel (path-traversal under root). The pre-fix code unconditionally
// joined the caller-supplied `name` argument onto the repo root and called
// os.Stat / os.ReadFile, allowing a "../../../etc/passwd"-style request to
// reach files outside the model allowlist. The fix routes the join through
// libvmaf.ValidatePath so the lookup is bounded to AllowedRoots().

package main

import (
	"strings"
	"testing"
)

// TestDescribeModelRejectsTraversal exercises the post-fix invariant: a
// caller-supplied path that resolves outside libvmaf.AllowedRoots() must NOT
// be silently opened. The pre-fix implementation would Stat /etc/passwd via
// "../../../etc/passwd"; the fix makes the candidate lookup fall through to
// the in-repo model/ walker which returns a "not found" error instead.
func TestDescribeModelRejectsTraversal(t *testing.T) {
	cases := []string{
		"../../../etc/passwd",
		"../../../../etc/shadow",
		"/etc/passwd",
		"/proc/self/environ",
	}
	for _, name := range cases {
		t.Run(name, func(t *testing.T) {
			_, err := describeModel(name)
			if err == nil {
				t.Fatalf("describeModel(%q) returned nil error; expected rejection", name)
			}
			// The error should come from the fallback "not found" or the
			// allowlist gate, not from a successful sneak-open.
			msg := err.Error()
			if !strings.Contains(msg, "not found") && !strings.Contains(msg, "allowlist") {
				t.Fatalf("describeModel(%q) unexpected error %q; expected not-found/allowlist gate", name, msg)
			}
		})
	}
}

// TestDescribeModelRejectsEmptyName confirms the explicit empty-name guard in
// handleDescribeModel still holds — defence in depth around the ValidatePath
// hardening.
func TestDescribeModelRejectsEmptyName(t *testing.T) {
	_, err := handleDescribeModel(t.Context(), map[string]any{"name": ""})
	if err == nil {
		t.Fatalf("handleDescribeModel({name:''}) returned nil error; expected rejection")
	}
	if !strings.Contains(err.Error(), "required") {
		t.Fatalf("unexpected error %q; expected 'name is required'", err.Error())
	}
}
