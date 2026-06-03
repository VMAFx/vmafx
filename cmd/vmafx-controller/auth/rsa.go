// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/auth/rsa.go — RS256 signature verification helpers.
//
// Separated from middleware.go to make the crypto path easy to audit.
//
// ADR-0794: multi-tenant auth gateway.

package auth

import (
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/pem"
	"fmt"
)

// verifyRS256 verifies an RS256 JWT signature.
// message is the bytes of "<header_b64>.<payload_b64>".
// sig is the decoded (raw bytes) signature from the JWT's third segment.
func verifyRS256(message, sig []byte, pub *rsa.PublicKey) error {
	h := sha256.Sum256(message)
	return rsa.VerifyPKCS1v15(pub, crypto.SHA256, h[:], sig)
}

// GenerateTestKeyPair creates an RSA-2048 key pair for use in tests.
// It must NOT be called in production paths.
func GenerateTestKeyPair() (*rsa.PrivateKey, *rsa.PublicKey, error) {
	priv, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return nil, nil, fmt.Errorf("auth test: generate RSA key: %w", err)
	}
	return priv, &priv.PublicKey, nil
}

// MarshalPublicKeyPEM serialises an RSA public key as PEM-encoded PKIX.
func MarshalPublicKeyPEM(pub *rsa.PublicKey) ([]byte, error) {
	der, err := x509.MarshalPKIXPublicKey(pub)
	if err != nil {
		return nil, err
	}
	return pem.EncodeToMemory(&pem.Block{Type: "PUBLIC KEY", Bytes: der}), nil
}
