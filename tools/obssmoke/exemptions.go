// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package main

// exemptions are the dashboard queries ("<dashboard title>: <where>") that
// cannot return data in the Compose example, each with the reason. An
// exempted query that returns data, or one no dashboard has any more, fails
// the smoke test.
var exemptions = map[string]string{
	"VMAFx Nodes and devices: panel GPU memory in use": "needs a GPU: the example's node is CPU-only, so it reports no device memory",
}
