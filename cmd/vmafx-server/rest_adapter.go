// SPDX-License-Identifier: EUPL-1.2
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
	"fmt"
	"io"
	"log/slog"
	"net/http"

	"google.golang.org/grpc/status"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/gen/go/oapi"
	"github.com/VMAFx/vmafx/internal/app/scoringservice"
	"github.com/VMAFx/vmafx/pkg/observability"
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
		scoringservice.WriteJSON(a.log, w, http.StatusInternalServerError,
			oapi.ErrorResponse{Error: fmt.Sprintf("health check failed: %v", err)})
		return
	}
	msg := resp.GetMessage()
	if msg == "" {
		msg = "ok"
	}
	scoringservice.WriteJSON(a.log, w, http.StatusOK, oapi.HealthResponse{Status: oapi.Ok})
}

// GetReady implements oapi.ServerInterface.GetReady — GET /v1/ready.
func (a *restAdapter) GetReady(w http.ResponseWriter, r *http.Request) {
	if a.grpc.scorer == nil {
		reason := "scorer not initialised"
		scoringservice.WriteJSON(a.log, w, http.StatusServiceUnavailable, oapi.ReadyResponse{
			Status: oapi.NotReady,
			Reason: &reason,
		})
		return
	}
	scoringservice.WriteJSON(a.log, w, http.StatusOK, oapi.ReadyResponse{Status: oapi.Ready})
}

// ScoreVideoPair implements oapi.ServerInterface.ScoreVideoPair — POST /v1/score.
//
// The body is the contract's ScoreRequest (proto field names, as the OpenAPI
// schema spells them, options included); it goes through grpcServer.Score, so
// the concurrency cap and the scoring path are gRPC's, and the response is the
// proto ScoreResponse with its provenance.
func (a *restAdapter) ScoreVideoPair(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxScoreRequestBodyBytes))
	req := &vmafxv1.ScoreRequest{}
	if err == nil {
		err = requestJSONOptions.Unmarshal(body, req)
	}
	if err != nil {
		scoringservice.WriteJSON(a.log, w, http.StatusBadRequest,
			oapi.ErrorResponse{Error: fmt.Sprintf("invalid JSON body: %v", err)})
		return
	}
	resp, err := a.grpc.Score(r.Context(), req)
	if err != nil {
		observability.RouteLogger(r.Context(), a.log, http.MethodPost, "/v1/score").Error(
			"rest ScoreVideoPair: grpc.Score failed", observability.FieldError, err)
		scoringservice.WriteJSON(a.log, w, httpStatusOf(err),
			oapi.ErrorResponse{Error: status.Convert(err).Message()})
		return
	}
	writeProtoJSON(a.log, w, resp)
}
