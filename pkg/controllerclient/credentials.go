// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// pkg/controllerclient/credentials.go — the credentials every client of the
// vmafx-controller's gRPC API presents: TLS transport credentials and a bearer
// token (ADR-1524 for the node, ADR-1569 for the operator).
//
// One implementation for every caller (HISS-19): vmafx-node and
// vmafx-operator read the same keys through their golusoris config tree (env
// prefix VMAFX_, "." delimiter; the underscore-bearing leaves are listed in
// CompoundKeys):
//
//	VMAFX_CONTROLLER_TLS          -> controller.tls          "true" dials with TLS (system roots unless a CA file is set).
//	VMAFX_CONTROLLER_CA_FILE      -> controller.ca_file      PEM bundle that verifies the controller certificate.
//	VMAFX_CONTROLLER_SERVER_NAME  -> controller.server_name  TLS server name override.
//	VMAFX_CONTROLLER_TOKEN_FILE   -> controller.token_file   File holding the bearer token, re-read on every call.
//	VMAFX_CONTROLLER_TOKEN        -> controller.token        Bearer token given inline.
//
// The token file is the refresh path: it is read again for every RPC, so a
// rotated Kubernetes projected token or Secret, or a file a sidecar rewrites,
// applies without a restart. A token that is a JWT whose "exp" has passed is
// refused before the call with an error naming the file, so a token source
// that stopped refreshing shows up as that, not as a bare Unauthenticated.

// Package controllerclient holds the client-side credentials of the
// vmafx-controller gRPC API.
package controllerclient

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials"
)

// CompoundKeys are the underscore-bearing leaf keys of Load.
//
// Deprecated: the vmafx binaries take their CompoundKeys from the generated
// config_keys.gen.go of each binary (api/vmafx-platform.toml, ADR-2350 D13),
// which holds these keys; this list stays for API compatibility.
var CompoundKeys = []string{
	"controller.ca_file",
	"controller.server_name",
	"controller.token_file",
}

// Getter is the read side of a config tree (*config.Config); tests pass a map.
type Getter interface {
	Get(path string) string
}

// Credentials are the TLS and bearer-token settings of a controller client.
type Credentials struct {
	TLS        bool
	CAFile     string
	ServerName string
	TokenFile  string
	Token      string
}

// Load reads and validates the controller.* credential keys.
func Load(cfg Getter) (Credentials, error) {
	out := Credentials{
		CAFile:     strings.TrimSpace(cfg.Get("controller.ca_file")),
		ServerName: strings.TrimSpace(cfg.Get("controller.server_name")),
		TokenFile:  strings.TrimSpace(cfg.Get("controller.token_file")),
		Token:      strings.TrimSpace(cfg.Get("controller.token")),
	}
	var errs []error
	if raw := strings.TrimSpace(cfg.Get("controller.tls")); raw != "" {
		v, err := strconv.ParseBool(raw)
		if err != nil {
			errs = append(errs, fmt.Errorf("controller.tls=%q is not a boolean", raw))
		}
		out.TLS = v
	}
	errs = append(errs, out.Validate()...)
	if len(errs) > 0 {
		return Credentials{}, errors.Join(errs...)
	}
	return out, nil
}

// Validate lists the combinations that cannot be honoured.
func (c Credentials) Validate() []error {
	var errs []error
	if c.Token != "" && c.TokenFile != "" {
		errs = append(errs, errors.New("set controller.token or controller.token_file, not both"))
	}
	if !c.TLS && (c.CAFile != "" || c.ServerName != "") {
		errs = append(errs, errors.New("controller.ca_file and controller.server_name need controller.tls=true"))
	}
	return errs
}

// HasToken reports whether a bearer token source is configured.
func (c Credentials) HasToken() bool { return c.Token != "" || c.TokenFile != "" }

// DialOptions returns the dial options for these credentials: TLS transport
// credentials when TLS is on (the caller's plaintext default otherwise) and
// the bearer token as per-RPC credentials when one is configured.
func (c Credentials) DialOptions() ([]grpc.DialOption, error) {
	var opts []grpc.DialOption
	creds, err := c.TransportCredentials()
	if err != nil {
		return nil, err
	}
	if creds != nil {
		opts = append(opts, grpc.WithTransportCredentials(creds))
	}
	if c.HasToken() {
		opts = append(opts, grpc.WithPerRPCCredentials(c.Bearer()))
	}
	return opts, nil
}

// TransportCredentials returns the TLS credentials, or nil for plaintext.
func (c Credentials) TransportCredentials() (credentials.TransportCredentials, error) {
	if !c.TLS {
		return nil, nil
	}
	tlsCfg := &tls.Config{MinVersion: tls.VersionTLS13, ServerName: c.ServerName}
	if c.CAFile != "" {
		pem, err := os.ReadFile(c.CAFile)
		if err != nil {
			return nil, fmt.Errorf("read controller CA file: %w", err)
		}
		pool := x509.NewCertPool()
		if !pool.AppendCertsFromPEM(pem) {
			return nil, fmt.Errorf("controller CA file %s holds no PEM certificate", c.CAFile)
		}
		tlsCfg.RootCAs = pool
	}
	return credentials.NewTLS(tlsCfg), nil
}

// Bearer returns the per-RPC credentials of the configured token source.
func (c Credentials) Bearer() *BearerCredentials {
	return &BearerCredentials{token: c.Token, tokenFile: c.TokenFile, requireTLS: c.TLS, now: time.Now}
}

// BearerCredentials implements credentials.PerRPCCredentials.
type BearerCredentials struct {
	token      string
	tokenFile  string
	requireTLS bool
	now        func() time.Time
}

// GetRequestMetadata returns the authorization header for one RPC.
func (b *BearerCredentials) GetRequestMetadata(_ context.Context, _ ...string) (map[string]string, error) {
	tok, err := b.load()
	if err != nil {
		return nil, err
	}
	return map[string]string{"authorization": "Bearer " + tok}, nil
}

// RequireTransportSecurity makes gRPC refuse to send the token over a
// plaintext connection when TLS was asked for.
func (b *BearerCredentials) RequireTransportSecurity() bool { return b.requireTLS }

// load returns the inline token or the current content of the token file,
// refusing an empty file and an expired JWT.
func (b *BearerCredentials) load() (string, error) {
	tok, where := b.token, "controller.token"
	if b.tokenFile != "" {
		raw, err := os.ReadFile(b.tokenFile)
		if err != nil {
			return "", fmt.Errorf("read controller token file: %w", err)
		}
		tok, where = strings.TrimSpace(string(raw)), "controller token file "+b.tokenFile
		if tok == "" {
			return "", fmt.Errorf("%s is empty", where)
		}
	}
	if exp, ok := jwtExpiry(tok); ok && !b.now().Before(exp) {
		return "", fmt.Errorf("%s holds a token that expired at %s; whatever writes it did not refresh it",
			where, exp.UTC().Format(time.RFC3339))
	}
	return tok, nil
}

// jwtExpiry returns the "exp" claim of a JWT, unverified; ok is false for a
// token that is not a JWT or has no numeric exp (it is then sent as is and
// the controller decides).
func jwtExpiry(tok string) (time.Time, bool) {
	parts := strings.Split(tok, ".")
	if len(parts) != 3 {
		return time.Time{}, false
	}
	payload, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return time.Time{}, false
	}
	var claims struct {
		Exp *json.Number `json:"exp"`
	}
	if json.Unmarshal(payload, &claims) != nil || claims.Exp == nil {
		return time.Time{}, false
	}
	secs, err := claims.Exp.Float64()
	if err != nil {
		return time.Time{}, false
	}
	return time.Unix(int64(secs), 0), true
}
