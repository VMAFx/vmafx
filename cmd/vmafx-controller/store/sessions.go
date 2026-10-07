// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/store/sessions.go — node sessions and heartbeats.

package store

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store/pgdb"
)

// tokenBytes is the entropy of a session token.
const tokenBytes = 32

// RegisterParams describes a node that opens a session.
type RegisterParams struct {
	TenantID   string
	NodeID     string
	Capability json.RawMessage
	TTL        time.Duration
}

// Session is an opened node session. Token is returned once; the store keeps
// only its SHA-256.
type Session struct {
	ID       uuid.UUID
	TenantID string
	NodeID   string
	Token    string
}

// SessionRef names a session in a node's later calls.
type SessionRef struct {
	ID       uuid.UUID
	TenantID string
	Token    string
}

// RegisterSession opens a session for a node of p.TenantID that expires
// after p.TTL unless a heartbeat renews it.
func (s *Postgres) RegisterSession(ctx context.Context, p RegisterParams) (Session, error) {
	if p.NodeID == "" {
		return Session{}, fmt.Errorf("%w: empty node id", ErrInvalid)
	}
	ttl, err := seconds(p.TTL, "session TTL")
	if err != nil {
		return Session{}, err
	}
	capability := p.Capability
	if len(capability) == 0 {
		capability = json.RawMessage("{}")
	}
	if !json.Valid(capability) {
		return Session{}, fmt.Errorf("%w: capability is not valid JSON", ErrInvalid)
	}
	sess, digest, err := newSession(p)
	if err != nil {
		return Session{}, err
	}
	err = s.inTenant(ctx, p.TenantID, func(ctx context.Context, q *pgdb.Queries) error {
		return q.InsertSession(ctx, pgdb.InsertSessionParams{
			ID: sess.ID, TenantID: p.TenantID, NodeID: p.NodeID, TokenSha256: digest[:],
			Capability: capability, SessionSeconds: ttl,
		})
	})
	if err != nil {
		return Session{}, fmt.Errorf("store: register session: %w", err)
	}
	return sess, nil
}

// newSession draws the ID and token of a session.
func newSession(p RegisterParams) (Session, [sha256.Size]byte, error) {
	id, err := uuid.NewV7()
	if err != nil {
		return Session{}, [sha256.Size]byte{}, fmt.Errorf("store: session id: %w", err)
	}
	raw := make([]byte, tokenBytes)
	if _, err := rand.Read(raw); err != nil {
		return Session{}, [sha256.Size]byte{}, fmt.Errorf("store: session token: %w", err)
	}
	token := base64.RawURLEncoding.EncodeToString(raw)
	return Session{ID: id, TenantID: p.TenantID, NodeID: p.NodeID, Token: token}, tokenDigest(token), nil
}

// tokenDigest is what the store keeps of a token.
func tokenDigest(token string) [sha256.Size]byte {
	return sha256.Sum256([]byte(token))
}

// liveSession returns the session ref names, locked for the transaction, or
// ErrSessionInvalid.
func liveSession(ctx context.Context, q *pgdb.Queries, ref SessionRef) (pgdb.NodeSession, error) {
	digest := tokenDigest(ref.Token)
	row, err := q.LiveSession(ctx, pgdb.LiveSessionParams{ID: ref.ID, TenantID: ref.TenantID, TokenSha256: digest[:]})
	if errors.Is(err, pgx.ErrNoRows) {
		return pgdb.NodeSession{}, ErrSessionInvalid
	}
	if err != nil {
		return pgdb.NodeSession{}, fmt.Errorf("store: read session: %w", err)
	}
	return row, nil
}

// HeartbeatParams renews a session and the leases of the jobs it runs.
type HeartbeatParams struct {
	Session    SessionRef
	Running    []uuid.UUID
	SessionTTL time.Duration
	LeaseTTL   time.Duration
}

// Heartbeat renews the session and the leases of the jobs in p.Running that
// the session holds, and returns those of p.Running that were cancelled, in
// the order of p.Running. An unknown, expired or foreign session is
// ErrSessionInvalid and renews nothing.
func (s *Postgres) Heartbeat(ctx context.Context, p HeartbeatParams) ([]uuid.UUID, error) {
	if len(p.Running) > MaxHeartbeatJobs {
		return nil, fmt.Errorf("%w: %d running jobs, at most %d", ErrInvalid, len(p.Running), MaxHeartbeatJobs)
	}
	sessionTTL, err := seconds(p.SessionTTL, "session TTL")
	if err != nil {
		return nil, err
	}
	leaseTTL, err := seconds(p.LeaseTTL, "lease TTL")
	if err != nil {
		return nil, err
	}
	var cancelled []uuid.UUID
	err = s.inTenant(ctx, p.Session.TenantID, func(ctx context.Context, q *pgdb.Queries) error {
		if terr := touchSession(ctx, q, p.Session, sessionTTL); terr != nil {
			return terr
		}
		if len(p.Running) == 0 {
			return nil
		}
		if _, eerr := q.ExtendLeases(ctx, pgdb.ExtendLeasesParams{
			LeaseSeconds: leaseTTL, TenantID: p.Session.TenantID, SessionID: p.Session.ID, Ids: p.Running,
		}); eerr != nil {
			return fmt.Errorf("store: extend leases: %w", eerr)
		}
		ids, cerr := q.CancelledAmong(ctx, pgdb.CancelledAmongParams{TenantID: p.Session.TenantID, Ids: p.Running})
		if cerr != nil {
			return fmt.Errorf("store: read cancelled jobs: %w", cerr)
		}
		cancelled = inOrder(p.Running, ids)
		return nil
	})
	return cancelled, err
}

// touchSession renews a live session or reports ErrSessionInvalid.
func touchSession(ctx context.Context, q *pgdb.Queries, ref SessionRef, ttl float64) error {
	digest := tokenDigest(ref.Token)
	n, err := q.TouchSession(ctx, pgdb.TouchSessionParams{
		SessionSeconds: ttl, ID: ref.ID, TenantID: ref.TenantID, TokenSha256: digest[:],
	})
	if err != nil {
		return fmt.Errorf("store: renew session: %w", err)
	}
	if n != 1 {
		return ErrSessionInvalid
	}
	return nil
}

// inOrder returns the members of found in the order they have in order.
func inOrder(order, found []uuid.UUID) []uuid.UUID {
	set := make(map[uuid.UUID]struct{}, len(found))
	for _, id := range found {
		set[id] = struct{}{}
	}
	out := make([]uuid.UUID, 0, len(found))
	for _, id := range order {
		if _, ok := set[id]; ok {
			out = append(out, id)
		}
	}
	return out
}
