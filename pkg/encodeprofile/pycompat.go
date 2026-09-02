// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/encodeprofile/pycompat.go — the CPython semantics this port has to
// reproduce to keep its output byte-identical: value coercion (float() / int()
// / str()), pathlib's path normalisation, and a stable sort.
//
// These live in one file so the parity-critical rules are reviewable together
// rather than scattered through the domain code.

package encodeprofile

import (
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
	"github.com/VMAFx/vmafx/pkg/pyjson"
)

// toFloat is CPython's float() over the JSON value shapes a profile can hold.
func toFloat(v any) (float64, bool) {
	switch t := v.(type) {
	case json.Number:
		f, err := t.Float64()
		return f, err == nil
	case float64:
		return t, true
	case int:
		return float64(t), true
	case int64:
		return float64(t), true
	case bool:
		if t {
			return 1, true
		}
		return 0, true
	case string:
		f, err := strconv.ParseFloat(strings.TrimSpace(t), 64)
		return f, err == nil
	default:
		return 0, false
	}
}

// toInt is CPython's int(): floats truncate toward zero, strings must be an
// integer literal, None raises.
func toInt(v any) (int, bool) {
	switch t := v.(type) {
	case json.Number:
		if i, err := strconv.ParseInt(t.String(), 10, 64); err == nil {
			return int(i), true
		}
		f, err := t.Float64()
		if err != nil {
			return 0, false
		}
		return int(math.Trunc(f)), true
	case float64:
		return int(math.Trunc(t)), true
	case int:
		return t, true
	case int64:
		return int(t), true
	case bool:
		if t {
			return 1, true
		}
		return 0, true
	case string:
		i, err := strconv.ParseInt(strings.TrimSpace(t), 10, 64)
		return int(i), err == nil
	default:
		return 0, false
	}
}

// finiteFloatOr is encoder_profile._finite_float: CPython's float() with a
// caller-chosen default for anything uncoercible or non-finite.
func finiteFloatOr(v any, def float64) float64 {
	f, ok := toFloat(v)
	if !ok || math.IsNaN(f) || math.IsInf(f, 0) {
		return def
	}
	return f
}

// pyStrOr renders v the way CPython's str() would, returning def for a value
// that Python would have short-circuited on (None or absent, both of which
// hit the `or ""` fallbacks in encoder_profile).
func pyStrOr(v any, def string) string {
	switch t := v.(type) {
	case nil:
		return def
	case string:
		if t == "" {
			return def
		}
		return t
	case bool:
		if t {
			return "True"
		}
		return def
	case json.Number:
		if i, err := strconv.ParseInt(t.String(), 10, 64); err == nil {
			if i == 0 {
				return def
			}
			return strconv.FormatInt(i, 10)
		}
		if f, err := t.Float64(); err == nil {
			if f == 0 {
				return def
			}
			return pyjson.FloatRepr(f)
		}
		return t.String()
	default:
		return fmt.Sprint(v)
	}
}

// truthy is CPython's bool() over the JSON value shapes a profile can hold.
func truthy(v any) bool {
	switch t := v.(type) {
	case nil:
		return false
	case bool:
		return t
	case string:
		return t != ""
	case json.Number:
		f, err := t.Float64()
		return err == nil && f != 0
	case float64:
		return t != 0
	case int:
		return t != 0
	case []any:
		return len(t) > 0
	case map[string]any:
		return len(t) > 0
	default:
		return true
	}
}

// pathString normalises s the way str(pathlib.PurePosixPath(s)) does.
//
// This matters because the emitted ffmpeg argv contains str(req.source) and
// str(req.output): a profile carrying "clips//a/./ref.yuv" renders as
// "clips/a/ref.yuv" in Python's --dry-run output, and anything else is a
// visible diff.
//
// filepath.Clean is close but not equivalent: it also collapses "..", which
// pathlib deliberately does NOT (str(PurePosixPath("a/../b")) == "a/../b",
// because resolving it would change meaning across symlinks).
func pathString(s string) string {
	if s == "" {
		return ""
	}

	// PurePosixPath keeps exactly two leading slashes ("//net/share"), and
	// collapses three or more to one.
	prefix := ""
	switch {
	case strings.HasPrefix(s, "//") && !strings.HasPrefix(s, "///"):
		prefix = "//"
	case strings.HasPrefix(s, "/"):
		prefix = "/"
	}

	parts := make([]string, 0, 8)
	for _, seg := range strings.Split(s, "/") {
		if seg == "" || seg == "." {
			continue
		}
		parts = append(parts, seg)
	}

	joined := strings.Join(parts, "/")
	if prefix != "" {
		return prefix + joined
	}
	if joined == "" {
		// str(PurePosixPath("")) and str(PurePosixPath(".")) are both ".".
		return "."
	}
	return joined
}

// pathSuffix returns the final extension of a path, the way
// pathlib.PurePath.suffix does: the dot and everything after the LAST dot of
// the final component, but only when that component has a non-empty stem
// (".bashrc" has no suffix; "a.tar.gz" has ".gz").
func pathSuffix(p string) string {
	name := pathString(p)
	if i := strings.LastIndex(name, "/"); i >= 0 {
		name = name[i+1:]
	}
	if name == "" || name == "." || name == ".." {
		return ""
	}
	i := strings.LastIndex(name, ".")
	if i <= 0 || i == len(name)-1 {
		// No dot, a leading dot (dotfile), or a trailing dot: no suffix.
		// pathlib also reports no suffix for a name ending in ".".
		return ""
	}
	return name[i:]
}

// DefaultPreset is encoder_profile._default_preset.
func DefaultPreset(codec string) string {
	return codecadapter.DefaultPreset(codec)
}

// NormalisePath exposes pathString to callers that echo a CLI path back in
// their output. argparse types --profile as a Path, so the Python CLI prints
// str(args.profile) — the normalised form — in its result JSON.
func NormalisePath(p string) string {
	return pathString(p)
}

// stableSortBy sorts in place with a stable less-than, matching CPython's
// list.sort.
func stableSortBy[T any](items []T, less func(a, b T) bool) {
	sort.SliceStable(items, func(i, j int) bool { return less(items[i], items[j]) })
}

// errorsAs is a thin wrapper so encode.go can use errors.As without importing
// errors solely for that (keeps the import list in encode.go focused on the
// subprocess machinery).
func errorsAs(err error, target any) bool {
	return errors.As(err, target)
}
