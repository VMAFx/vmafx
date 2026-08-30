// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// The bucket goldens below were produced by running the Python
// reference verbatim:
//
//	h = hashlib.sha256(f"vmaf-train-splits-v1:{key}".encode()).digest()
//	int.from_bytes(h[:8], "big") / (1 << 64)
//
// They are exact float64 values (%.17g), so the assertions below are
// bit-equality — not tolerance-based. A change here means the Go and
// Python servers would disagree about which rows belong to which split,
// which silently invalidates every cross-language model comparison.

package modeleval

import (
	"fmt"
	"math"
	"testing"
)

func TestBucketMatchesPythonExactly(t *testing.T) {
	cases := []struct {
		key    string
		bucket float64
		split  string
	}{
		{"", 0.077445882671784524, SplitTest},
		{"0", 0.99409911827354469, SplitTrain},
		{"1", 0.99390274486012509, SplitTrain},
		{"42", 0.56491157196515074, SplitTrain},
		{"clip_0001", 0.53812798600153688, SplitTrain},
		{"clip_0002", 0.42723000797720628, SplitTrain},
		{"src01_hrc00", 0.98876164119489862, SplitTrain},
		{"a", 0.72175205009009002, SplitTrain},
		{"z", 0.35567669495683152, SplitTrain},
		{"netflix/BigBuckBunny", 0.8896995631036394, SplitTrain},
		{"ümläut", 0.84887957896425048, SplitTrain},
		{"1234567890", 0.011570990086954414, SplitTest},
	}
	for _, tc := range cases {
		t.Run(fmt.Sprintf("key=%q", tc.key), func(t *testing.T) {
			// Bit-exact: any drift re-partitions every feature cache.
			if got := Bucket(tc.key); got != tc.bucket {
				t.Errorf("Bucket(%q) = %.17g, want %.17g", tc.key, got, tc.bucket)
			}
			if got := SplitOf(tc.key); got != tc.split {
				t.Errorf("SplitOf(%q) = %q, want %q", tc.key, got, tc.split)
			}
		})
	}
}

func TestBucketIsInUnitInterval(t *testing.T) {
	for i := range 2000 {
		k := fmt.Sprintf("clip_%06d", i)
		b := Bucket(k)
		if math.IsNaN(b) || b < 0 || b >= 1 {
			t.Fatalf("Bucket(%q) = %v, outside [0, 1)", k, b)
		}
	}
}

// TestSplitDistribution pins the 10/10/80 partition. The exact counts
// come from running the Python `which()` over the same 20000 keys, so
// this asserts the two implementations agree in aggregate, not merely
// that the proportions look plausible.
func TestSplitDistribution(t *testing.T) {
	counts := map[string]int{}
	for i := range 20000 {
		counts[SplitOf(fmt.Sprintf("clip_%06d", i))]++
	}
	want := map[string]int{SplitTrain: 15940, SplitVal: 2014, SplitTest: 2046}
	for k, v := range want {
		if counts[k] != v {
			t.Errorf("split %q count = %d, want %d (full: %v)", k, counts[k], v, counts)
		}
	}
}

// TestSplitOfIsExhaustive proves every key lands in exactly one split,
// so no row is silently dropped when the three splits are unioned.
func TestSplitOfIsExhaustive(t *testing.T) {
	for i := range 5000 {
		k := fmt.Sprintf("k%d", i)
		s := SplitOf(k)
		if s != SplitTrain && s != SplitVal && s != SplitTest {
			t.Fatalf("SplitOf(%q) = %q, not one of train/val/test", k, s)
		}
	}
}

func TestValidateSplit(t *testing.T) {
	cases := []struct {
		name    string
		split   string
		wantErr bool
	}{
		{"train", SplitTrain, false},
		{"val", SplitVal, false},
		{"test", SplitTest, false},
		{"all", SplitAll, false},
		{"empty is not a split", "", true},
		{"unknown", "foo", true},
		{"case sensitive", "Test", true},
		{"whitespace", " test", true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := ValidateSplit(tc.split)
			if tc.wantErr != (err != nil) {
				t.Fatalf("ValidateSplit(%q) error = %v, wantErr %v", tc.split, err, tc.wantErr)
			}
		})
	}
}

// TestValidateSplitMessageMatchesPython pins the error text, which the
// MCP tool surfaces verbatim to clients.
func TestValidateSplitMessageMatchesPython(t *testing.T) {
	err := ValidateSplit("foo")
	if err == nil {
		t.Fatal("expected an error")
	}
	want := `split must be one of ('train', 'val', 'test', 'all'); got "foo"`
	if err.Error() != want {
		t.Errorf("message = %q, want %q", err.Error(), want)
	}
}
