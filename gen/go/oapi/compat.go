// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// gen/go/oapi/compat.go — names earlier stubs exported that the generator no
// longer emits. Hand-written; oapi-codegen does not touch this file.
//
// ADR-0797: vmafx-server OpenAPI REST contract. HISS-14: the Go API of the
// stubs only grows. bitdepth was an enum of 8, 10, 12 and 16 and is now the
// range 8 to 16, so the generator emits no enum type; the spec names the
// field's type with x-go-type (go_type in core/api/vmafx.toml) and this file
// declares it. Without the bitdepth enum the precision constants lost their
// type-name prefix.

package oapi

// ScoreOptionsBitdepth is a bit depth of raw .yuv input, 8 to 16.
type ScoreOptionsBitdepth int32

// Bit depths of the former bitdepth enum; every value from 8 to 16 is valid.
const (
	ScoreOptionsBitdepthN10 ScoreOptionsBitdepth = 10
	ScoreOptionsBitdepthN12 ScoreOptionsBitdepth = 12
	ScoreOptionsBitdepthN16 ScoreOptionsBitdepth = 16
	ScoreOptionsBitdepthN8  ScoreOptionsBitdepth = 8
)

// Valid reports whether e is a bit depth the API accepts, 8 to 16.
func (e ScoreOptionsBitdepth) Valid() bool {
	return e >= 8 && e <= 16
}

// Prefixed names of the ScoreOptionsPrecision values.
//
// Deprecated: use the generated unprefixed constants (Full, Legacy, Max, N1 to N17).
const (
	ScoreOptionsPrecisionFull   = Full
	ScoreOptionsPrecisionLegacy = Legacy
	ScoreOptionsPrecisionMax    = Max
	ScoreOptionsPrecisionN1     = N1
	ScoreOptionsPrecisionN2     = N2
	ScoreOptionsPrecisionN3     = N3
	ScoreOptionsPrecisionN4     = N4
	ScoreOptionsPrecisionN5     = N5
	ScoreOptionsPrecisionN6     = N6
	ScoreOptionsPrecisionN7     = N7
	ScoreOptionsPrecisionN8     = N8
	ScoreOptionsPrecisionN9     = N9
	ScoreOptionsPrecisionN10    = N10
	ScoreOptionsPrecisionN11    = N11
	ScoreOptionsPrecisionN12    = N12
	ScoreOptionsPrecisionN13    = N13
	ScoreOptionsPrecisionN14    = N14
	ScoreOptionsPrecisionN15    = N15
	ScoreOptionsPrecisionN16    = N16
	ScoreOptionsPrecisionN17    = N17
)
