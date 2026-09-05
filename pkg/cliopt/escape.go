// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

// Package cliopt builds the option strings the vmaf CLI's `--model` and
// `--feature` flags consume.
//
// Those flags take a colon-delimited list of `key=value` pairs; `:` separates
// pairs and the first `=` in a pair separates the key from the value. Any of
// those characters inside a value has to be backslash-escaped, or the parser
// splits the value at it — see ADR-1190 and docs/usage/cli.md
// ("Option-string grammar"). Callers that paste a user-supplied path into
// `path=<path>` must therefore escape the path first; before ADR-1190 there was
// no escape syntax at all and such a path was silently truncated at its first
// inner `=`.
package cliopt

import "strings"

// EscapeValue escapes s for use as the value half of a vmaf CLI option-string
// pair. A backslash, a colon and an equals sign each gain a leading backslash;
// every other byte is passed through unchanged.
//
// A '.' needs no escaping in a value — it only separates the feature name from
// the option name in a model feature overload key
// (`--model version=...:adm.adm_enhn_gain_limit=1.2`).
func EscapeValue(s string) string {
	if !strings.ContainsAny(s, `\:=`) {
		return s
	}
	var b strings.Builder
	b.Grow(len(s) + 8)
	for i := range len(s) {
		switch c := s[i]; c {
		case '\\', ':', '=':
			b.WriteByte('\\')
			b.WriteByte(c)
		default:
			b.WriteByte(c)
		}
	}
	return b.String()
}
