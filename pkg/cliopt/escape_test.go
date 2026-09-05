// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package cliopt

import "testing"

// The cases mirror the C-side regression tests in core/test/test_cli_parse.c:
// what EscapeValue emits here is what cli_split()/cli_unescape() must read back
// as the original string (ADR-1190).
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
		{"windows path", `C:\models\vmaf_v0.6.1.json`, `C\:\\models\\vmaf_v0.6.1.json`},
		{"unc path", `\\server\share\m.json`, `\\\\server\\share\\m.json`},
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

// unescape is the Go mirror of cli_unescape() in core/tools/cli_parse.cpp: it
// drops the backslash in \: \= \. and \\ and leaves every other backslash
// alone. Round-tripping through it pins EscapeValue to the parser's grammar.
func unescape(s string) string {
	out := make([]byte, 0, len(s))
	for i := 0; i < len(s); i++ {
		if s[i] == '\\' && i+1 < len(s) {
			switch s[i+1] {
			case ':', '=', '.', '\\':
				i++
			}
		}
		out = append(out, s[i])
	}
	return string(out)
}

func TestEscapeValueRoundTrips(t *testing.T) {
	t.Parallel()
	for _, in := range []string{
		"/models/vmaf_v0.6.1.json",
		"/a/dir=eq/m.json",
		"/a/dir:colon/m.json",
		`C:\models\vmaf_v0.6.1.json`,
		`\\server\share\m.json`,
		`weird=:\name`,
		"",
	} {
		if got := unescape(EscapeValue(in)); got != in {
			t.Errorf("unescape(EscapeValue(%q)) = %q, want round-trip", in, got)
		}
	}
}
