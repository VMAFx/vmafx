// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/bisect/parse_test.go — unit tests for parseVMAFXMLMean and the
// VMAFScoreFunc constructor's pure-go branches. Avoids spawning a real
// vmaf subprocess; the binary-spawn path is covered indirectly by the
// integration scenarios.

package bisect

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/VMAFx/vmafx/pkg/encoder"
)

// TestParseVMAFXMLMean_HappyPath covers the canonical XML form.
func TestParseVMAFXMLMean_HappyPath(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	xmlPath := filepath.Join(dir, "score.xml")
	payload := `<?xml version="1.0"?>
<VMAF>
  <pooled_metrics>
    <metric name="vif_scale0" min="0.1" max="1.0" mean="0.85"/>
    <metric name="vmaf" min="80.0" max="95.0" mean="87.654321" harmonic_mean="86.9"/>
  </pooled_metrics>
</VMAF>
`
	if err := os.WriteFile(xmlPath, []byte(payload), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	got, err := parseVMAFXMLMean(xmlPath)
	if err != nil {
		t.Fatalf("parseVMAFXMLMean: %v", err)
	}
	if got != 87.654321 {
		t.Errorf("mean = %v, want 87.654321", got)
	}
}

// TestParseVMAFXMLMean_FileMissing covers the read-error branch.
func TestParseVMAFXMLMean_FileMissing(t *testing.T) {
	t.Parallel()
	_, err := parseVMAFXMLMean("/no/such/file.xml")
	if err == nil {
		t.Fatal("expected error for missing file")
	}
}

// TestParseVMAFXMLMean_NoVMAFMetric covers the needle-not-found branch.
func TestParseVMAFXMLMean_NoVMAFMetric(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	xmlPath := filepath.Join(dir, "no-vmaf.xml")
	if err := os.WriteFile(xmlPath, []byte(`<VMAF><other_metric mean="1.0"/></VMAF>`), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	_, err := parseVMAFXMLMean(xmlPath)
	if err == nil {
		t.Fatal("expected error when vmaf metric absent")
	}
}

// TestParseVMAFXMLMean_NoMeanAttr covers the mean-prefix-not-found branch.
func TestParseVMAFXMLMean_NoMeanAttr(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	xmlPath := filepath.Join(dir, "no-mean.xml")
	// The vmaf needle is present but no mean= attribute follows it.
	if err := os.WriteFile(xmlPath, []byte(`<metric name="vmaf" min="0"/>`), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	_, err := parseVMAFXMLMean(xmlPath)
	if err == nil {
		t.Fatal("expected error when mean attr missing")
	}
}

// TestParseVMAFXMLMean_UnterminatedMean covers the no-closing-quote branch.
func TestParseVMAFXMLMean_UnterminatedMean(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	xmlPath := filepath.Join(dir, "bad.xml")
	if err := os.WriteFile(xmlPath, []byte(`<metric name="vmaf" mean="87.5`), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	_, err := parseVMAFXMLMean(xmlPath)
	if err == nil {
		t.Fatal("expected error for unterminated mean attribute")
	}
}

// TestParseVMAFXMLMean_NonNumericMean covers the parseFloat-error branch.
func TestParseVMAFXMLMean_NonNumericMean(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	xmlPath := filepath.Join(dir, "nonnum.xml")
	if err := os.WriteFile(xmlPath, []byte(`<metric name="vmaf" mean="not-a-number"/>`), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	_, err := parseVMAFXMLMean(xmlPath)
	if err == nil {
		t.Fatal("expected ParseFloat error")
	}
}

// TestVMAFScoreFunc_DefaultsBinaryName verifies VMAFScoreFunc returns a
// non-nil ScoreFunc when no binary path is given (the binary lookup is
// deferred until invocation).
func TestVMAFScoreFunc_DefaultsBinaryName(t *testing.T) {
	t.Parallel()
	fn := VMAFScoreFunc("")
	if fn == nil {
		t.Fatal("VMAFScoreFunc('') returned nil")
	}
}

// TestVMAFScoreFunc_PropagatesBinaryError verifies the returned func
// surfaces an error when the vmaf binary cannot run.
func TestVMAFScoreFunc_PropagatesBinaryError(t *testing.T) {
	t.Parallel()
	fn := VMAFScoreFunc("/definitely/no/such/vmaf/binary")
	_, err := fn("ref.yuv", "dis.yuv")
	if err == nil {
		t.Fatal("expected error when vmaf binary unavailable")
	}
}

// TestApplyDefaults_MaxIter exercises the MaxIter default branch.
func TestApplyDefaults_MaxIter(t *testing.T) {
	t.Parallel()
	p := Params{TargetVMAF: 90.0, MaxIter: 0}
	enc := &applyDefaultsMockEnc{}
	p.applyDefaults(enc)
	if p.MaxIter != DefaultMaxIter {
		t.Errorf("MaxIter = %d, want %d", p.MaxIter, DefaultMaxIter)
	}
}

// TestApplyDefaults_PreservesNonZeroWindow verifies a caller-supplied
// CRF window is preserved.
func TestApplyDefaults_PreservesNonZeroWindow(t *testing.T) {
	t.Parallel()
	p := Params{CRFLo: 5, CRFHi: 30, MaxIter: 8}
	enc := &applyDefaultsMockEnc{}
	p.applyDefaults(enc)
	if p.CRFLo != 5 || p.CRFHi != 30 {
		t.Errorf("CRF window mutated: (%d, %d)", p.CRFLo, p.CRFHi)
	}
	if p.MaxIter != 8 {
		t.Errorf("MaxIter = %d, want 8", p.MaxIter)
	}
}

// applyDefaultsMockEnc is a minimal encoder.Encoder stand-in used only
// for applyDefaults's CRFRange() call.
type applyDefaultsMockEnc struct{}

func (applyDefaultsMockEnc) Name() string         { return "mock" }
func (applyDefaultsMockEnc) CRFRange() (int, int) { return 2, 48 }
func (applyDefaultsMockEnc) Encode(string, encoder.EncodeParams) (encoder.EncodeResult, error) {
	return encoder.EncodeResult{}, nil
}
