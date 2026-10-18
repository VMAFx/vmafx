//go:build !secretsgate

// SPDX-FileCopyrightText: 2026 lusoris <lusoris@pm.me>
//
// SPDX-License-Identifier: EUPL-1.2

// Command gate is the standalone secrets history gate. This file is what a build without
// the secretsgate tag sees of it: a program that says how to run the gate.
package main

import (
	"fmt"
	"os"
)

func main() {
	fmt.Fprintln(os.Stderr, "the secrets history gate runs with its build tag: "+
		"go run -tags=secretsgate ./tools/secrets/gate")
	os.Exit(1)
}
