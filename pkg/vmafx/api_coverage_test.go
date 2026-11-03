// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

//go:build cgo

package vmafx

import (
	"go/ast"
	"go/parser"
	"go/token"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// wrappedFunctions lists the exported functions and methods api_gen.go
// declares: the surface every test file of this package must exercise
// (#2434: every wrapped function has a test against the Meson-built library).
// Methods are keyed by receiver and name, plain functions by name.
func wrappedFunctions(t *testing.T, file string) map[string]string {
	t.Helper()
	parsed, err := parser.ParseFile(token.NewFileSet(), file, nil, 0)
	if err != nil {
		t.Fatalf("parse %s: %v", file, err)
	}
	out := map[string]string{}
	for _, decl := range parsed.Decls {
		fn, ok := decl.(*ast.FuncDecl)
		if !ok || !fn.Name.IsExported() {
			continue
		}
		key := fn.Name.Name
		if fn.Recv != nil && len(fn.Recv.List) == 1 {
			key = receiverName(fn.Recv.List[0].Type) + "." + fn.Name.Name
		}
		out[key] = fn.Name.Name
	}
	return out
}

func receiverName(expr ast.Expr) string {
	if star, ok := expr.(*ast.StarExpr); ok {
		expr = star.X
	}
	if ident, ok := expr.(*ast.Ident); ok {
		return ident.Name
	}
	return "?"
}

// calledNames returns every function and method name a test file calls or
// takes as a method value.
func calledNames(t *testing.T, files []string) map[string]bool {
	t.Helper()
	used := map[string]bool{}
	for _, file := range files {
		parsed, err := parser.ParseFile(token.NewFileSet(), file, nil, 0)
		if err != nil {
			t.Fatalf("parse %s: %v", file, err)
		}
		ast.Inspect(parsed, func(n ast.Node) bool {
			switch e := n.(type) {
			case *ast.SelectorExpr:
				used[e.Sel.Name] = true
			case *ast.Ident:
				used[e.Name] = true
			}
			return true
		})
	}
	return used
}

// untested returns the wrapped functions whose name no test file mentions.
func untested(wrapped map[string]string, used map[string]bool) []string {
	var missing []string
	for key, name := range wrapped {
		if !used[name] {
			missing = append(missing, key)
		}
	}
	sort.Strings(missing)
	return missing
}

func TestEveryWrappedFunctionHasATest(t *testing.T) {
	files, err := filepath.Glob("*_test.go")
	if err != nil {
		t.Fatal(err)
	}
	var tests []string
	for _, f := range files {
		if f != "api_coverage_test.go" {
			tests = append(tests, f)
		}
	}
	wrapped := wrappedFunctions(t, "api_gen.go")
	if len(wrapped) < 40 {
		t.Fatalf("api_gen.go declares %d exported functions, the scan is broken", len(wrapped))
	}
	if missing := untested(wrapped, calledNames(t, tests)); len(missing) > 0 {
		t.Fatalf("wrapped functions no test calls: %s", strings.Join(missing, ", "))
	}
}

// TestUntestedFindsAMissingFunction plants a wrapped function no test calls and
// shows the check reports it, and stays quiet for one that is called.
func TestUntestedFindsAMissingFunction(t *testing.T) {
	wrapped := map[string]string{"Context.NeverCalled": "NeverCalled", "Model.Name": "Name"}
	got := untested(wrapped, map[string]bool{"Name": true})
	if len(got) != 1 || got[0] != "Context.NeverCalled" {
		t.Fatalf("untested = %v, want [Context.NeverCalled]", got)
	}
	if got := untested(wrapped, map[string]bool{"Name": true, "NeverCalled": true}); len(got) != 0 {
		t.Fatalf("untested = %v, want none", got)
	}
}
