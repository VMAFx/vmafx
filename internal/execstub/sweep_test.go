// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package execstub

import (
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// executableWrite is a call that creates or marks a file executable by mode:
// os.WriteFile, os.OpenFile or os.Chmod with an execute bit in an octal literal.
var executableWrite = regexp.MustCompile(`os\.(WriteFile|OpenFile|Chmod)\(.*\b0o?[0-7]?[1357][0-7]{2}\)`)

// maxGoFiles bounds the walk of the module (HISS-02).
const maxGoFiles = 20000

// moduleRoot is the directory of the go.mod above this package.
func moduleRoot(t *testing.T) string {
	t.Helper()
	dir, err := filepath.Abs(".")
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 16; i++ {
		if _, err := os.Stat(filepath.Join(dir, "go.mod")); err == nil {
			return dir
		}
		dir = filepath.Dir(dir)
	}
	t.Fatal("no go.mod above the package")
	return ""
}

// offenders lists the Go test files under root, outside this package, that
// write an executable without Write.
func offenders(t *testing.T, root string) []string {
	t.Helper()
	var found []string
	seen := 0
	err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() && path != root && (strings.HasPrefix(d.Name(), ".") || d.Name() == "testdata" || d.Name() == "node_modules") {
			return filepath.SkipDir
		}
		if d.IsDir() || !strings.HasSuffix(path, "_test.go") || filepath.Base(filepath.Dir(path)) == "execstub" {
			return nil
		}
		if seen++; seen > maxGoFiles {
			return errors.New("more Go test files than the walk allows")
		}
		text, err := os.ReadFile(path) // #nosec G304 -- a test file of this module
		if err != nil {
			return err
		}
		if executableWrite.Match(text) {
			rel, _ := filepath.Rel(root, path)
			found = append(found, filepath.ToSlash(rel))
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return found
}

// TestEveryStubGoesThroughWrite: no Go test of the module writes an
// executable with os.WriteFile, os.OpenFile or os.Chmod; Write holds the fork
// lock those do not (go.dev/issue/22315).
func TestEveryStubGoesThroughWrite(t *testing.T) {
	if found := offenders(t, moduleRoot(t)); len(found) != 0 {
		t.Errorf("test files writing an executable without execstub.Write: %v", found)
	}
}

// TestPlantedExecutableWriteIsFound: a planted os.WriteFile of a stub with
// mode 0o755 is named; a data file with mode 0o600 is not.
func TestPlantedExecutableWriteIsFound(t *testing.T) {
	root := t.TempDir()
	for name, body := range map[string]string{
		"bad_test.go":  "package x\nfunc f() { _ = os.WriteFile(p, []byte(s), 0o755) }\n",
		"good_test.go": "package x\nfunc f() { _ = os.WriteFile(p, []byte(s), 0o600) }\n",
	} {
		if err := os.WriteFile(filepath.Join(root, name), []byte(body), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	if found := offenders(t, root); len(found) != 1 || found[0] != "bad_test.go" {
		t.Errorf("offenders = %v, want [bad_test.go]", found)
	}
}
