// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package version_test

import (
	"testing"

	"github.com/VMAFx/vmafx/pkg/version"
)

func TestVersionNotEmpty(t *testing.T) {
	v := version.Version()
	if v == "" {
		t.Fatal("Version() must not return an empty string")
	}
}
