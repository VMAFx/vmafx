// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

// Package cliopt builds the option strings the vmaf CLI's `--model` and
// `--feature` flags consume.
//
// Those flags take a colon-delimited list of `key=value` pairs; `:` separates
// pairs and the first `=` in a pair separates the key from the value. A `:` or
// `=` inside a value has to be backslash-escaped, or the parser splits the
// value at it — see ADR-1190, ADR-1355 and docs/usage/cli.md ("Option-string
// grammar"). Callers that paste a user-supplied path into `path=<path>` must
// therefore escape the path first; before ADR-1190 there was no escape syntax
// at all and such a path was silently truncated at its first inner `=`.
package cliopt

import "strings"

// EscapeValue escapes s for use as the value half of a vmaf CLI option-string
// pair. The result reads back as s wherever it sits in the option string.
//
// A colon or an equals sign gains a leading backslash. A backslash is data in
// a value (ADR-1355), so `C:\models\m.json`, `..\m.json` and `\\server\share`
// keep theirs, except in a run that directly precedes a `:` or `=` or ends s:
// the parser reads such a run in pairs, so it is doubled. Every other byte is
// passed through unchanged.
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
	run := 0 // backslashes read but not yet written
	for i := range len(s) {
		switch c := s[i]; c {
		case '\\':
			run++
		case ':', '=':
			writeBackslashes(&b, 2*run+1)
			b.WriteByte(c)
			run = 0
		default:
			writeBackslashes(&b, run)
			b.WriteByte(c)
			run = 0
		}
	}
	writeBackslashes(&b, 2*run)
	return b.String()
}

func writeBackslashes(b *strings.Builder, n int) {
	for range n {
		b.WriteByte('\\')
	}
}
