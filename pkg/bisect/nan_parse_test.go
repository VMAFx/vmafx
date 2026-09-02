// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/bisect/nan_parse_test.go — regression coverage for
// parseVMAFXMLMean's non-finite rejection.
//
// strconv.ParseFloat accepts the literal tokens "NaN", "+Inf", "-Inf"
// without error. A corrupt vmaf XML output (one observed root cause:
// vmaf CLI emitting "NaN" when scoring against a zero-frame distorted
// file) would propagate a NaN VMAFScore into bisect.Sample, which then
// crashed json.MarshalIndent at the compare report-emit stage. The fix
// is to reject the corrupt mean at parse time so the bisect step
// records "score failed" rather than producing a Sample with a
// non-finite field.

package bisect

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestParseVMAFXMLMean_RejectsNaN(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	xmlPath := filepath.Join(dir, "nan.xml")
	if err := os.WriteFile(xmlPath, []byte(`<metric name="vmaf" mean="NaN"/>`), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	_, err := parseVMAFXMLMean(xmlPath)
	if err == nil {
		t.Fatal("expected NaN mean to be rejected, got nil error")
	}
	if !strings.Contains(err.Error(), "non-finite") {
		t.Errorf("error should mention non-finite, got: %v", err)
	}
}

func TestParseVMAFXMLMean_RejectsPositiveInf(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	xmlPath := filepath.Join(dir, "posinf.xml")
	if err := os.WriteFile(xmlPath, []byte(`<metric name="vmaf" mean="+Inf"/>`), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	_, err := parseVMAFXMLMean(xmlPath)
	if err == nil {
		t.Fatal("expected +Inf mean to be rejected, got nil error")
	}
}

func TestParseVMAFXMLMean_RejectsNegativeInf(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	xmlPath := filepath.Join(dir, "neginf.xml")
	if err := os.WriteFile(xmlPath, []byte(`<metric name="vmaf" mean="-Inf"/>`), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	_, err := parseVMAFXMLMean(xmlPath)
	if err == nil {
		t.Fatal("expected -Inf mean to be rejected, got nil error")
	}
}
