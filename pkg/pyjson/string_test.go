// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/pyjson/string_test.go — ensure_ascii=True escaping.
//
// Every `want` below is a verbatim json.dumps(s) result from CPython 3; the
// table is the union of the cases the four pre-consolidation encoders pinned
// independently (ADR-1137).

package pyjson

import "testing"

func TestEncodeString(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		in   string
		want string
	}{
		{"plain ascii", "libx264", `"libx264"`},
		{"quote and backslash", `a"b\c`, `"a\"b\\c"`},
		{"short escapes", "a\nb\tc\rd\be\ff", `"a\nb\tc\rd\be\ff"`},
		{"other control chars", "\x00\x1f", `"\u0000\u001f"`},
		{"other control char inline", "a\x01b", `"a\u0001b"`},
		{"tilde is the last literal byte", "~", `"~"`},
		{"del is escaped", "\x7f", `"\u007f"`},
		// CPython never escapes HTML metacharacters; encoding/json does
		// unless SetEscapeHTML(false).
		{"html metacharacters stay raw", "<a>&'</a>", `"<a>&'</a>"`},
		{"ffmpeg filter fragment", "scale=w=320:h=240", `"scale=w=320:h=240"`},
		{"latin-1 escaped", "caf\xc3\xa9", `"caf\u00e9"`},
		{"cjk escaped", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", `"\u65e5\u672c\u8a9e"`},
		{"em dash escaped", "a\xe2\x80\x94b", `"a\u2014b"`},
		{"astral plane uses a surrogate pair", "\U0001f3ac", `"\ud83c\udfac"`},
		{"emoji surrogate pair", "\U0001F600", `"\ud83d\ude00"`},
		{"invalid utf-8 byte becomes the replacement escape", "a\xffb", `"a\ufffdb"`},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := EncodeString(tc.in); got != tc.want {
				t.Errorf("EncodeString(%q) = %s, want %s", tc.in, got, tc.want)
			}
		})
	}
}
