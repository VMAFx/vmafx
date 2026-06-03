// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/swagger_ui.go — Swagger UI handler for vmafx-server.
//
// Mounts a self-contained Swagger UI at /swagger using the CDN-hosted
// swagger-ui-dist bundle (unpkg.com, pinned to v5.x).  The embedded OpenAPI
// spec is served under /swagger/spec.json from the in-binary blob generated
// by oapi-codegen so no filesystem access is required at runtime.
//
// The UI is intentionally read-only: it renders the spec and lets operators
// explore the API, but it does not expose a "try it out" live-execution path
// to production deployments.  To enable try-it-out, set the environment
// variable VMAFX_SWAGGER_TRY_IT_OUT=1; the UI HTML sets the supportedSubmitMethods
// list accordingly.
//
// ADR-0797: vmafx-server OpenAPI REST contract.

//go:build cgo

package main

import (
	"fmt"
	"log/slog"
	"net/http"
	"os"

	"github.com/VMAFx/vmafx/gen/go/oapi"
)

const (
	// swaggerUIVersion pins the CDN-hosted swagger-ui-dist bundle.
	// Bump deliberately — do not auto-update; pin must be reviewed for
	// breaking changes in Swagger UI's HTML/JS API surface.
	swaggerUIVersion = "5.18.2"

	// swaggerUIBaseURL is the CDN path prefix for the pinned version.
	swaggerUIBaseURL = "https://unpkg.com/swagger-ui-dist@" + swaggerUIVersion
)

// swaggerUIHTML is the Swagger UI index page template.
// %s slots: (1) spec URL, (2) optional supportedSubmitMethods override.
const swaggerUIHTML = `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <title>VMAFX Scoring API — Swagger UI</title>
  <link rel="stylesheet" href="%s/swagger-ui.css">
</head>
<body>
<div id="swagger-ui"></div>
<script src="%s/swagger-ui-bundle.js"></script>
<script src="%s/swagger-ui-standalone-preset.js"></script>
<script>
window.onload = function() {
  window.ui = SwaggerUIBundle({
    url: '/swagger/spec.json',
    dom_id: '#swagger-ui',
    deepLinking: true,
    presets: [
      SwaggerUIBundle.presets.apis,
      SwaggerUIStandalonePreset
    ],
    layout: 'StandaloneLayout',
    supportedSubmitMethods: [%s],
    validatorUrl: null
  });
};
</script>
</body>
</html>`

// swaggerTryItOut returns the try-it-out method list based on the
// VMAFX_SWAGGER_TRY_IT_OUT environment variable.
func swaggerTryItOut() string {
	if os.Getenv("VMAFX_SWAGGER_TRY_IT_OUT") == "1" {
		return `'get','put','post','delete','options','head','patch','trace'`
	}
	// Empty list disables the "Try it out" button.
	return ""
}

// registerSwaggerUI mounts the Swagger UI routes on mux:
//
//   - GET /swagger         — HTML UI page
//   - GET /swagger/        — alias (redirect)
//   - GET /swagger/spec.json — embedded OpenAPI spec (JSON)
//
// The spec is served from the in-binary blob embedded by oapi-codegen at
// gen/go/oapi/vmafx_server_v1.gen.go.
func registerSwaggerUI(mux *http.ServeMux, log *slog.Logger) {
	mux.HandleFunc("/swagger/spec.json", handleSwaggerSpec(log))
	mux.HandleFunc("/swagger", handleSwaggerIndex(log))
	mux.HandleFunc("/swagger/", handleSwaggerIndex(log))
}

// handleSwaggerSpec returns a handler that serves the embedded OpenAPI spec
// as application/json.
func handleSwaggerSpec(log *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		specJSON, err := oapi.GetSpecJSON()
		if err != nil {
			log.Error("swagger spec decode failed", "error", err)
			http.Error(w, "internal error reading spec", http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "public, max-age=3600")
		w.WriteHeader(http.StatusOK)
		if _, err := w.Write(specJSON); err != nil {
			log.Error("swagger spec write failed", "error", err)
		}
	}
}

// handleSwaggerIndex returns a handler that serves the Swagger UI HTML page.
func handleSwaggerIndex(log *slog.Logger) http.HandlerFunc {
	html := fmt.Sprintf(swaggerUIHTML,
		swaggerUIBaseURL, // stylesheet href
		swaggerUIBaseURL, // bundle script src
		swaggerUIBaseURL, // standalone-preset script src
		swaggerTryItOut(),
	)
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		// Redirect /swagger/ → /swagger for canonical URL.
		if r.URL.Path == "/swagger/" {
			http.Redirect(w, r, "/swagger", http.StatusMovedPermanently)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.WriteHeader(http.StatusOK)
		if _, err := fmt.Fprint(w, html); err != nil {
			log.Error("swagger ui write failed", "error", err)
		}
	}
}
