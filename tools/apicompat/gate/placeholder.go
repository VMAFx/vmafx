//go:build !apicompatgate

// SPDX-FileCopyrightText: 2026 lusoris <lusoris@pm.me>
//
// SPDX-License-Identifier: EUPL-1.2

// Command gate is the Go API compatibility gate of the api:public-contract facet. This file is
// what a build without the apicompatgate tag sees of it: a program that only says how to run
// the gate. It gives the directory one buildable package, so a pre-commit hook that vets or
// lints it by directory finds Go files, while the gate itself (main.go) stays out of every
// ./... pattern of the module.
package main

import (
	"fmt"
	"os"
)

func main() {
	fmt.Fprintln(os.Stderr, "the API compatibility gate builds from its file: "+
		"go run tools/apicompat/gate/main.go [-base=<rev>]")
	os.Exit(1)
}
