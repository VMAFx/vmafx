// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/auth/authtest/issuer.go — an RS256 token issuer with a
// JWKS endpoint, for tests of the controller's auth gateway.
//
// The auth package's own tests and the controller's end-to-end tests (which
// boot the production fx graph with auth enabled) mint tokens the same way;
// this package is that one implementation. It is imported only from _test.go
// files.
//
// ADR-0794: multi-tenant auth gateway. ADR-1518: gRPC authorisation.

// Package authtest mints RS256 JWTs and serves their JWKS for tests.
package authtest

import (
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"maps"
	"math/big"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// keyBits is the RSA modulus size of generated test keys: the smallest the
// controller accepts (minRSAKeyBits in the auth package).
const keyBits = 2048

// Issuer is a test identity provider: one RSA key and an HTTP server that
// serves its JWKS. Its URL is both the issuer ("iss") and the JWKS endpoint.
type Issuer struct {
	Key    *rsa.PrivateKey
	Kid    string
	Server *httptest.Server
}

// NewIssuer generates a key and starts the JWKS server; the server stops when
// the test ends.
func NewIssuer(t testing.TB) *Issuer {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, keyBits)
	if err != nil {
		t.Fatalf("authtest: generate RSA key: %v", err)
	}
	iss := &Issuer{Key: key, Kid: "authtest-key-1"}
	doc := JWKS(map[string]*rsa.PublicKey{iss.Kid: &key.PublicKey})
	iss.Server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		if _, werr := w.Write([]byte(doc)); werr != nil {
			t.Errorf("authtest: write JWKS: %v", werr)
		}
	}))
	t.Cleanup(iss.Server.Close)
	return iss
}

// URL is the issuer identifier and the JWKS endpoint of the issuer.
func (i *Issuer) URL() string { return i.Server.URL }

// Token signs claims with the issuer's key. "iss" defaults to URL() and "exp"
// to one hour from now; every other claim is taken as given.
func (i *Issuer) Token(t testing.TB, claims map[string]any) string {
	t.Helper()
	payload := make(map[string]any, len(claims)+2)
	payload["iss"] = i.URL()
	payload["exp"] = time.Now().Add(time.Hour).Unix()
	maps.Copy(payload, claims)
	return Sign(t, i.Key, "RS256", i.Kid, payload)
}

// Sign returns the compact serialisation of a JWT whose header carries alg and
// kid and whose payload is claims, signed RS256 (PKCS #1 v1.5, SHA-256) with
// key whatever alg the header names.
func Sign(t testing.TB, key *rsa.PrivateKey, alg, kid string, claims map[string]any) string {
	t.Helper()
	hdr, err := json.Marshal(map[string]string{"alg": alg, "typ": "JWT", "kid": kid})
	if err != nil {
		t.Fatalf("authtest: marshal JWT header: %v", err)
	}
	body, err := json.Marshal(claims)
	if err != nil {
		t.Fatalf("authtest: marshal JWT claims: %v", err)
	}
	input := base64.RawURLEncoding.EncodeToString(hdr) + "." + base64.RawURLEncoding.EncodeToString(body)
	sum := sha256.Sum256([]byte(input))
	sig, err := rsa.SignPKCS1v15(rand.Reader, key, crypto.SHA256, sum[:])
	if err != nil {
		t.Fatalf("authtest: sign JWT: %v", err)
	}
	return input + "." + base64.RawURLEncoding.EncodeToString(sig)
}

// JWK renders one RSA public key as a JWKS entry.
func JWK(kid string, pub *rsa.PublicKey) string {
	n := base64.RawURLEncoding.EncodeToString(pub.N.Bytes())
	e := base64.RawURLEncoding.EncodeToString(big.NewInt(int64(pub.E)).Bytes())
	return fmt.Sprintf(`{"kty":"RSA","kid":%q,"n":%q,"e":%q}`, kid, n, e)
}

// JWKS renders a JWKS document holding the given keys, keyed by kid.
func JWKS(keys map[string]*rsa.PublicKey) string {
	entries := make([]string, 0, len(keys))
	for kid, pub := range keys {
		entries = append(entries, JWK(kid, pub))
	}
	return `{"keys":[` + strings.Join(entries, ",") + `]}`
}
