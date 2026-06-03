// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/rest_adapter.go — REST → gRPC adapter for vmafx-server.
//
// restAdapter implements oapi.ServerInterface by translating the oapi-codegen
// request / response types into the gRPC-generated proto types and delegating
// to grpcServer.  This keeps a single source of truth: all business logic
// (scoring, metrics, logging) lives in grpcServer; restAdapter is pure
// translation.
//
// ADR-0797: vmafx-server OpenAPI REST contract.

//go:build cgo

package main

import (
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"

	"github.com/VMAFx/vmafx/gen/go/oapi"
	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
)

// restAdapter translates oapi.ServerInterface calls into gRPC handler calls.
type restAdapter struct {
	grpc *grpcServer
	log  *slog.Logger
}

// newRestAdapter creates a restAdapter backed by the given grpcServer.
func newRestAdapter(grpc *grpcServer, log *slog.Logger) *restAdapter {
	return &restAdapter{grpc: grpc, log: log}
}

// GetHealth implements oapi.ServerInterface.GetHealth — GET /v1/health.
func (a *restAdapter) GetHealth(w http.ResponseWriter, r *http.Request) {
	resp, err := a.grpc.Health(r.Context(), &vmafxv1.HealthRequest{})
	if err != nil {
		writeOAPIJSON(w, http.StatusInternalServerError,
			oapi.ErrorResponse{Error: fmt.Sprintf("health check failed: %v", err)})
		return
	}
	msg := resp.GetMessage()
	if msg == "" {
		msg = "ok"
	}
	writeOAPIJSON(w, http.StatusOK, oapi.HealthResponse{Status: oapi.Ok})
}

// GetReady implements oapi.ServerInterface.GetReady — GET /v1/ready.
func (a *restAdapter) GetReady(w http.ResponseWriter, r *http.Request) {
	if a.grpc.scorer == nil {
		reason := "scorer not initialised"
		writeOAPIJSON(w, http.StatusServiceUnavailable, oapi.ReadyResponse{
			Status: oapi.NotReady,
			Reason: &reason,
		})
		return
	}
	writeOAPIJSON(w, http.StatusOK, oapi.ReadyResponse{Status: oapi.Ready})
}

// ScoreVideoPair implements oapi.ServerInterface.ScoreVideoPair — POST /v1/score.
//
// The handler decodes the oapi.ScoreRequest JSON body, calls grpcServer.Score
// via the proto types, and encodes an oapi.ScoreResponse.
func (a *restAdapter) ScoreVideoPair(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var body oapi.ScoreRequest
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeOAPIJSON(w, http.StatusBadRequest,
			oapi.ErrorResponse{Error: fmt.Sprintf("invalid JSON body: %v", err)})
		return
	}

	model := ""
	if body.Model != nil {
		model = *body.Model
	}

	grpcResp, err := a.grpc.Score(r.Context(), &vmafxv1.ScoreRequest{
		Reference: body.Reference,
		Distorted: body.Distorted,
		Model:     model,
	})
	if err != nil {
		a.log.Error("rest ScoreVideoPair: grpc.Score failed", "error", err)
		writeOAPIJSON(w, http.StatusInternalServerError,
			oapi.ErrorResponse{Error: err.Error()})
		return
	}

	writeOAPIJSON(w, http.StatusOK, oapi.ScoreResponse{
		Score:    grpcResp.GetScore(),
		Features: grpcResp.GetFeatures(),
	})
}

// writeOAPIJSON serialises v as JSON with the given HTTP status code.
// Reuses the same encoding settings as the existing writeJSON helper so
// response formatting is consistent across HTTP and REST transports.
func writeOAPIJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(v)
}
