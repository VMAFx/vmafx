// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package codecadapter

import (
	"slices"
	"testing"

	"github.com/VMAFx/vmafx/pkg/encoderregistry"
)

// TestRegistryRowsMatchAdapters holds the adapter table and the implemented
// rows of the encoder registry (ADR-3388) to the same codec set: a row
// without an argv shape here, or an adapter without a row, fails by name.
func TestRegistryRowsMatchAdapters(t *testing.T) {
	t.Parallel()
	rows, err := encoderregistry.Implemented()
	if err != nil {
		t.Fatalf("Implemented: %v", err)
	}
	ids := make([]string, 0, len(rows))
	for _, row := range rows {
		ids = append(ids, row.ID)
	}
	slices.Sort(ids)
	if got := Known(); !slices.Equal(got, ids) {
		t.Fatalf("Known() = %v, registry implemented rows = %v", got, ids)
	}
}

// TestFillFromRegistryRejectsGaps checks the negative path: a name with no
// registry row, and a row that is not implemented, both refuse to fill.
func TestFillFromRegistryRejectsGaps(t *testing.T) {
	t.Parallel()
	if err := fillFromRegistry(&Adapter{Name: "no-such-encoder"}); err == nil {
		t.Error("fillFromRegistry accepted a name without a registry row")
	}
	doc, err := encoderregistry.Load()
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	for _, row := range doc.Encoders {
		if row.Adapter == nil {
			if err := fillFromRegistry(&Adapter{Name: row.ID}); err == nil {
				t.Errorf("fillFromRegistry accepted gap row %q", row.ID)
			}
			return
		}
	}
	t.Log("registry has no gap row yet; the gap branch is covered once one lands")
}

// TestFilledFactsBoundary pins one boundary per fact family: the inclusive
// window edges validate, one past them does not.
func TestFilledFactsBoundary(t *testing.T) {
	t.Parallel()
	a, err := Get("libsvtav1")
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	lo, hi := a.QualityRange[0], a.QualityRange[1]
	for _, q := range []int{lo, hi} {
		if err := a.Validate("medium", q); err != nil {
			t.Errorf("Validate(medium, %d): %v", q, err)
		}
	}
	for _, q := range []int{lo - 1, hi + 1} {
		if err := a.Validate("medium", q); err == nil {
			t.Errorf("Validate(medium, %d) accepted a value outside [%d, %d]", q, lo, hi)
		}
	}
}
