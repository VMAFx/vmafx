// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/pyjson/string.go — json.dumps(ensure_ascii=True) string literals.

package pyjson

import (
	"fmt"
	"strings"
	"unicode/utf16"
	"unicode/utf8"
)

// replacementEscape is U+FFFD spelled the way ensure_ascii=True would.
const replacementEscape = `\ufffd`

// EncodeString renders s as the JSON string literal json.dumps produces under
// its default ensure_ascii=True.
//
// encoding/json is not usable here for two reasons, both of which produce
// bytes Python never would: it HTML-escapes <, > and & (a source path or an
// ffmpeg filter fragment containing & would diverge), and it emits non-ASCII
// as raw UTF-8 where Python escapes every rune past U+007E to a \uXXXX
// sequence, with a surrogate pair above the BMP.
//
// The six short escapes (\" \\ \b \f \n \r \t) are used where CPython uses
// them; the remaining C0 controls and DEL are \u00XX. An invalid UTF-8 byte
// decodes as U+FFFD and is emitted as its escape, which keeps the payload
// well-formed rather than truncating it (CPython would have raised on decode,
// so such input can never have come from the Python side).
func EncodeString(s string) string {
	var sb strings.Builder
	sb.Grow(len(s) + 2)
	sb.WriteByte('"')
	for _, r := range s {
		writeRune(&sb, r)
	}
	sb.WriteByte('"')
	return sb.String()
}

// writeRune appends one rune of a string literal body.
func writeRune(sb *strings.Builder, r rune) {
	switch r {
	case '"':
		sb.WriteString(`\"`)
	case '\\':
		sb.WriteString(`\\`)
	case '\b':
		sb.WriteString(`\b`)
	case '\f':
		sb.WriteString(`\f`)
	case '\n':
		sb.WriteString(`\n`)
	case '\r':
		sb.WriteString(`\r`)
	case '\t':
		sb.WriteString(`\t`)
	default:
		writeUnescapedOrUnicode(sb, r)
	}
}

// writeUnescapedOrUnicode appends a rune that has no short escape: printable
// ASCII verbatim, everything else as a \uXXXX escape (surrogate-paired past
// U+FFFF).
func writeUnescapedOrUnicode(sb *strings.Builder, r rune) {
	switch {
	case r >= 0x20 && r <= 0x7e:
		// #nosec G115 -- the guard bounds r to printable ASCII, so the
		// narrowing conversion cannot truncate.
		sb.WriteByte(byte(r))
	case r == utf8.RuneError:
		sb.WriteString(replacementEscape)
	case r > 0xffff:
		hi, lo := utf16.EncodeRune(r)
		fmt.Fprintf(sb, `\u%04x\u%04x`, hi, lo)
	default:
		fmt.Fprintf(sb, `\u%04x`, r)
	}
}
