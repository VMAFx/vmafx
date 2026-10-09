/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/**
 * @file signer_identity.h
 * @brief Who may sign a tiny model: the signing certificate in a Sigstore
 *        bundle must name VMAFx's release workflow and VMAFx's GitHub owner ID.
 *
 * Rationale (ADR-2985):
 *   `cosign verify-blob` checks the signature, the certificate chain, the
 *   transparency-log entry and an identity regular expression on the
 *   certificate's subject alternative name. The name is the organisation's
 *   login, which GitHub frees when an organisation is renamed, so a later
 *   owner of the name could get a certificate with the same name. Fulcio also
 *   writes the owner's numeric ID into extension 1.3.6.1.4.1.57264.1.17, which
 *   a new owner of the name cannot have, and cosign 3.1.3 has no flag for it.
 *   This module reads that extension and the subject alternative name from
 *   the one certificate a bundle holds, before cosign runs on the same bytes.
 *
 * Accepted bundles hold exactly one `rawBytes` field (a Sigstore bundle v0.3,
 * which `cosign sign-blob` 3.x writes) and no `\u` escape, so the certificate
 * read here is the certificate cosign verifies.
 */

#ifndef LIBVMAF_DNN_SIGNER_IDENTITY_H_
#define LIBVMAF_DNN_SIGNER_IDENTITY_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The `--certificate-identity-regexp` passed to cosign (RE2 syntax, anchored
 * at both ends). It accepts the subject alternative names that
 * vmaf_dnn_signer_identity_allowed() accepts: VMAFx's supply-chain workflow on a
 * release tag, or on master for a manual re-publication of a tag.
 */
#define VMAF_DNN_SIGNER_IDENTITY_REGEXP                                                            \
    "^https://github\\.com/VMAFx/vmafx/\\.github/workflows/supply-chain\\.yml"                     \
    "@refs/(heads/master|tags/v[0-9][0-9A-Za-z.+-]*)$"

/** The `--certificate-oidc-issuer` passed to cosign. */
#define VMAF_DNN_SIGNER_OIDC_ISSUER "https://token.actions.githubusercontent.com"

/** GitHub's numeric ID of the VMAFx organisation (Fulcio extension .1.17). */
#define VMAF_DNN_SIGNER_OWNER_ID "288567244"

/**
 * Check the signing certificate of the Sigstore bundle in @p bundle
 * (@p len bytes, JSON). Returns 0 when the bundle holds exactly one
 * certificate, its subject alternative name is allowed and its owner ID is
 * VMAF_DNN_SIGNER_OWNER_ID; -EBADMSG when the bundle or the certificate cannot
 * be read; -EPERM when the certificate names another identity or owner.
 * Does not verify any signature: cosign does.
 */
int vmaf_dnn_signer_check_bundle(const char *bundle, size_t len);

/**
 * Check a DER-encoded X.509 certificate (@p len bytes) as
 * vmaf_dnn_signer_check_bundle() checks the bundle's certificate.
 */
int vmaf_dnn_signer_check_certificate(const unsigned char *der, size_t len);

/**
 * Return 1 when the subject alternative name @p san (@p len bytes, not
 * NUL-terminated) is one VMAF_DNN_SIGNER_IDENTITY_REGEXP accepts, else 0.
 */
int vmaf_dnn_signer_identity_allowed(const char *san, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_DNN_SIGNER_IDENTITY_H_ */
