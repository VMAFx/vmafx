// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/backend/command_test.go — the migrate and
// import-sqlite commands.

package backend_test

import (
	"bytes"
	"context"
	"log/slog"
	"strings"
	"testing"

	"github.com/google/uuid"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/backend"
)

func env(vars map[string]string) func(string) string {
	return func(k string) string { return vars[k] }
}

func TestCommandIgnoresOtherArguments(t *testing.T) {
	t.Parallel()
	for _, args := range [][]string{{"vmafx-controller"}, {"vmafx-controller", "--version"}, {"vmafx-controller", "serve"}} {
		if handled, _ := backend.Command(args, env(nil), &bytes.Buffer{}, slog.New(slog.DiscardHandler)); handled {
			t.Fatalf("%v was taken as a one-shot command", args)
		}
	}
}

func TestCommandsRefuseAMissingDSNOrFile(t *testing.T) {
	t.Parallel()
	cases := map[string][]string{
		"migrate without a DSN":     {"vmafx-controller", "migrate"},
		"import without --from":     {"vmafx-controller", "import-sqlite"},
		"import with a stray value": {"vmafx-controller", "import-sqlite", "--from", "x.db", "extra"},
		"import without a DSN":      {"vmafx-controller", "import-sqlite", "--from", "x.db"},
	}
	for name, args := range cases {
		var out bytes.Buffer
		handled, code := backend.Command(args, env(nil), &out, slog.New(slog.DiscardHandler))
		if !handled || code != backend.ExitUsage {
			t.Errorf("%s: handled=%v code=%d out=%q, want usage", name, handled, code, out.String())
		}
	}
}

func TestMigrateAndImportCommands(t *testing.T) {
	t.Parallel()
	b, db := newBackend(t)
	vars := env(map[string]string{backend.DSNEnv: db.DSN})
	logger := slog.New(slog.DiscardHandler)
	var out bytes.Buffer
	if handled, code := backend.Command([]string{"vmafx-controller", "migrate"}, vars, &out, logger); !handled || code != backend.ExitOK {
		t.Fatalf("migrate: code=%d out=%q", code, out.String())
	}
	if _, err := db.Pool.Exec(context.Background(), "SELECT 1 FROM river_job LIMIT 1"); err != nil {
		t.Fatalf("migrate did not create River's tables: %v", err)
	}
	id := uuid.NewString()
	path := queueFile(t, []legacyRow{{id: id, status: "pending", tenant: "t1"}})
	out.Reset()
	handled, code := backend.Command([]string{"vmafx-controller", "import-sqlite", "--from", path}, vars, &out, logger)
	if !handled || code != backend.ExitOK || !strings.Contains(out.String(), "imported 1 jobs (1 back in the queue)") {
		t.Fatalf("import: code=%d out=%q", code, out.String())
	}
	if _, err := b.Get(context.Background(), "t1", id); err != nil {
		t.Fatalf("imported job: %v", err)
	}
}

func TestImportCommandRefusesAnUnmigratedDatabase(t *testing.T) {
	t.Parallel()
	_, db := newBackend(t)
	if _, err := db.Pool.Exec(context.Background(), "DROP TABLE schema_migrations"); err != nil {
		t.Fatalf("drop bookkeeping: %v", err)
	}
	var out bytes.Buffer
	vars := env(map[string]string{backend.DSNEnv: db.DSN})
	path := queueFile(t, nil)
	_, code := backend.Command([]string{"vmafx-controller", "import-sqlite", "--from", path}, vars, &out, slog.New(slog.DiscardHandler))
	if code != backend.ExitError || !strings.Contains(out.String(), "run vmafx-controller migrate first") {
		t.Fatalf("code=%d out=%q", code, out.String())
	}
}
