// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/config_precedence_test.go — pins the precedence documented
// in docs/server/configuration.md: built-in defaults < config file < environment.
// The env transform splits every underscore, so each key is also pinned in its
// dotted form (issue #1251).

//go:build cgo

package main

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"

	"github.com/golusoris/golusoris/core/config"
)

// precedenceConfig loads a config with a YAML file layer under the production
// env contract (prefix, delimiter, compound keys of serverEnvOptions).
func precedenceConfig(t *testing.T, yaml string) *config.Config {
	t.Helper()
	opts := serverEnvOptions(false)
	if yaml != "" {
		path := filepath.Join(t.TempDir(), "vmafx.yaml")
		if err := os.WriteFile(path, []byte(yaml), 0o600); err != nil {
			t.Fatalf("write config file: %v", err)
		}
		opts.Files = []string{path}
	}
	cfg, err := config.New(opts)
	if err != nil {
		t.Fatalf("config.New: %v", err)
	}
	return cfg
}

func TestConfigPrecedenceDefaultFileEnv(t *testing.T) {
	const file = "max:\n  concurrent:\n    scores: 3\nhttp:\n  addr: \":1111\"\nmodel:\n  dir: /from/file\n"

	t.Run("default when nothing is set", func(t *testing.T) {
		limiter, err := provideScoreLimiter(precedenceConfig(t, ""))
		if err != nil {
			t.Fatal(err)
		}
		if limiter.Max() != int64(runtime.NumCPU()) {
			t.Errorf("limiter max = %d, want NumCPU %d", limiter.Max(), runtime.NumCPU())
		}
	})

	t.Run("file beats default", func(t *testing.T) {
		limiter, err := provideScoreLimiter(precedenceConfig(t, file))
		if err != nil {
			t.Fatal(err)
		}
		if limiter.Max() != 3 {
			t.Errorf("limiter max = %d, want file value 3", limiter.Max())
		}
	})

	t.Run("env beats file", func(t *testing.T) {
		t.Setenv("VMAFX_MAX_CONCURRENT_SCORES", "7")
		t.Setenv("VMAFX_HTTP_ADDR", ":2222")
		cfg := precedenceConfig(t, file)
		limiter, err := provideScoreLimiter(cfg)
		if err != nil {
			t.Fatal(err)
		}
		if limiter.Max() != 7 {
			t.Errorf("limiter max = %d, want env value 7", limiter.Max())
		}
		if got := cfg.Get("http.addr"); got != ":2222" {
			t.Errorf("http.addr = %q, want env value :2222", got)
		}
		if got := cfg.Get("model.dir"); got != "/from/file" {
			t.Errorf("model.dir = %q, want file value for a key env leaves alone", got)
		}
	})
}

// TestConfigEnvNameSplitsEveryUnderscore pins the trap: VMAFX_MAX_CONCURRENT_SCORES
// is the key max.concurrent.scores, and the underscore spelling is never a key.
func TestConfigEnvNameSplitsEveryUnderscore(t *testing.T) {
	t.Setenv("VMAFX_MAX_CONCURRENT_SCORES", "5")
	cfg := precedenceConfig(t, "")
	if got := cfg.Int("max.concurrent.scores"); got != 5 {
		t.Errorf("max.concurrent.scores = %d, want 5", got)
	}
	if got := cfg.Int("max_concurrent_scores"); got != 0 {
		t.Errorf("max_concurrent_scores = %d, want 0 (the underscore spelling is not a key)", got)
	}
}

// TestServerLoadsNoConfigFileOrFlags pins the documented fact that production
// vmafx-server has an environment layer only.
func TestServerLoadsNoConfigFileOrFlags(t *testing.T) {
	if files := serverEnvOptions(true).Files; len(files) != 0 {
		t.Errorf("serverEnvOptions declares config files %v; docs/server/configuration.md says none", files)
	}
}
