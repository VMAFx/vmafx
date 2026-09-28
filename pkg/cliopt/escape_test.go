// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package cliopt

import (
	"strings"
	"testing"
)

// The cases mirror the C-side regression tests in core/test/test_cli_parse.c:
// what EscapeValue emits here is what cli_split()/cli_unescape_value() must
// read back as the original string (ADR-1190, ADR-1355).
func TestEscapeValue(t *testing.T) {
	t.Parallel()
	for _, tc := range []struct {
		name string
		in   string
		want string
	}{
		{"plain posix path", "/models/vmaf_v0.6.1.json", "/models/vmaf_v0.6.1.json"},
		{"inner equals", "/a/dir=eq/m.json", `/a/dir\=eq/m.json`},
		{"inner colon", "/a/dir:colon/m.json", `/a/dir\:colon/m.json`},
		{"windows path", `C:\models\vmaf_v0.6.1.json`, `C\:\models\vmaf_v0.6.1.json`},
		{"unc path", `\\server\share\m.json`, `\\server\share\m.json`},
		{"relative parent path", `..\..\models\m.json`, `..\..\models\m.json`},
		{"dot directory", `C:\models\.cache\m.json`, `C\:\models\.cache\m.json`},
		{"backslash before colon", `a\:b`, `a\\\:b`},
		{"backslash before equals", `a\=b`, `a\\\=b`},
		{"trailing backslash", `C:\out\`, `C\:\out\\`},
		{"dot is not escaped", "/a/m.json", "/a/m.json"},
		{"empty", "", ""},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := EscapeValue(tc.in); got != tc.want {
				t.Errorf("EscapeValue(%q) = %q, want %q", tc.in, got, tc.want)
			}
		})
	}
}

// unescape is the Go mirror of cli_unescape_value() in
// core/tools/cli_parse.cpp. A backslash is data unless it belongs to a run
// that directly precedes ':' or '=' or ends the value; such a run is read in
// pairs, and a lone backslash left over escapes the ':' or '=' (or, at the
// end, stands for itself). Round-tripping through it pins EscapeValue to the
// parser's grammar.
func unescape(s string) string {
	out := make([]byte, 0, len(s))
	for i := 0; i < len(s); {
		if s[i] != '\\' {
			out = append(out, s[i])
			i++
			continue
		}
		run := 0
		for i+run < len(s) && s[i+run] == '\\' {
			run++
		}
		keep := run
		switch {
		case i+run == len(s):
			keep = run/2 + run%2
		case s[i+run] == ':' || s[i+run] == '=':
			keep = run / 2
		}
		out = append(out, strings.Repeat(`\`, keep)...)
		i += run
	}
	return string(out)
}

// split mirrors cli_split(): it returns the value token that ends at the first
// ':' preceded by an even run of backslashes.
func split(s string) string {
	for i := 0; i < len(s); i++ {
		switch s[i] {
		case '\\':
			i++ // the escaped byte is skipped
		case ':':
			return s[:i]
		}
	}
	return s
}

func TestEscapeValueRoundTrips(t *testing.T) {
	t.Parallel()
	for _, in := range []string{
		"/models/vmaf_v0.6.1.json",
		"/a/dir=eq/m.json",
		"/a/dir:colon/m.json",
		`C:\models\vmaf_v0.6.1.json`,
		`\\server\share\m.json`,
		`..\..\models\m.json`,
		`C:\models\.cache\m.json`,
		`weird=:\name`,
		`a\:b\=c\\:d`,
		`C:\out\`,
		`\\`,
		"",
	} {
		escaped := EscapeValue(in)
		if got := unescape(escaped); got != in {
			t.Errorf("unescape(EscapeValue(%q)) = %q, want round-trip", in, got)
		}
		// A following pair must not change what the value reads back as.
		if got := unescape(split(escaped + ":name=x")); got != in {
			t.Errorf("value %q followed by another pair read back as %q", in, got)
		}
	}
}
