// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

// Package codec is a transitional alias of pkg/codecadapter.
//
// The codec-adapter registry has one implementation, pkg/codecadapter
// (ADR-1137); the parallel port's second registry that lived here was
// deleted. This package remains only because pkg/tune/sidecar and
// cmd/vmafx-tune/cmd/sidecar.go still import it and those files are owned
// by an in-flight PR (#1187, the sidecar Python-parity fix); repointing them
// here would conflict with it. Once #1187 lands, the sidecar imports move to
// pkg/codecadapter and this package is deleted. Do not add anything to it.
package codec

import "github.com/VMAFx/vmafx/pkg/codecadapter"

// Adapter is codecadapter.Adapter.
type Adapter = codecadapter.Adapter

// Get is codecadapter.Get: the registered adapter for name, or an error naming
// the unknown codec.
func Get(name string) (*Adapter, error) {
	return codecadapter.Get(name)
}

// Known is codecadapter.Known: every registered codec name, sorted.
func Known() []string {
	return codecadapter.Known()
}
