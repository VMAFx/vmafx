/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  ADR-2985: who may sign a tiny model. The signing certificate of a bundle
 *  must name VMAFx's supply-chain workflow and carry VMAFx's GitHub owner ID
 *  (Fulcio extension 1.3.6.1.4.1.57264.1.17); a name with a prefix or a
 *  suffix, another workflow, or the right name under another owner fails.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "signer_fixtures.h"
#include "signer_identity.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit (ADR-1138). */

#define TEST_MAX_SAN 1024u
#define TEST_MAX_BUNDLE 16384u
#define TEST_SHORT_BUNDLE 8192u

static int san_allowed(const char *san)
{
    return vmaf_dnn_signer_identity_allowed(san, strnlen(san, TEST_MAX_SAN));
}

static int bundle_status(const char *bundle)
{
    return vmaf_dnn_signer_check_bundle(bundle, strnlen(bundle, TEST_MAX_BUNDLE));
}

static char *test_identity_accepts_release_and_master(void)
{
    mu_assert("release tag", san_allowed("https://github.com/VMAFx/vmafx/.github/workflows/"
                                         "supply-chain.yml@refs/tags/v1.0.0-rc.3"));
    mu_assert("master (manual re-publication)",
              san_allowed("https://github.com/VMAFx/vmafx/.github/workflows/"
                          "supply-chain.yml@refs/heads/master"));
    return NULL;
}

static char *test_identity_rejects_prefix_and_suffix(void)
{
    mu_assert("prefix", !san_allowed("https://evil.example/https://github.com/VMAFx/vmafx/"
                                     ".github/workflows/supply-chain.yml@refs/tags/v1.0.0"));
    mu_assert("suffix after the ref", !san_allowed("https://github.com/VMAFx/vmafx/.github/"
                                                   "workflows/supply-chain.yml@refs/heads/"
                                                   "master/evil"));
    mu_assert("suffix on the repository", !san_allowed("https://github.com/VMAFx/vmafx-evil/"
                                                       ".github/workflows/supply-chain.yml"
                                                       "@refs/tags/v1.0.0"));
    mu_assert("tag with a path", !san_allowed("https://github.com/VMAFx/vmafx/.github/"
                                              "workflows/supply-chain.yml@refs/tags/v1/x"));
    return NULL;
}

static char *test_identity_rejects_other_names(void)
{
    mu_assert("dot is not a wildcard", !san_allowed("https://githubXcom/VMAFx/vmafx/.github/"
                                                    "workflows/supply-chain.yml@refs/tags/v1"));
    mu_assert("other workflow", !san_allowed("https://github.com/VMAFx/vmafx/.github/"
                                             "workflows/evil.yml@refs/heads/master"));
    mu_assert("other branch", !san_allowed("https://github.com/VMAFx/vmafx/.github/workflows/"
                                           "supply-chain.yml@refs/heads/evil"));
    mu_assert("tag without a digit", !san_allowed("https://github.com/VMAFx/vmafx/.github/"
                                                  "workflows/supply-chain.yml@refs/tags/vx"));
    mu_assert("empty", !san_allowed(""));
    mu_assert("NULL", vmaf_dnn_signer_identity_allowed(NULL, 4) == 0);
    return NULL;
}

static char *test_bundle_release_certificate_accepted(void)
{
    mu_assert("v1.0.0-rc.3 release certificate", bundle_status(k_bundle_release) == 0);
    mu_assert("self-signed, right name and owner", bundle_status(k_bundle_master) == 0);
    return NULL;
}

static char *test_bundle_wrong_owner_rejected(void)
{
    mu_assert("right name, other owner ID", bundle_status(k_bundle_wrong_owner) == -EPERM);
    mu_assert("no owner ID extension", bundle_status(k_bundle_no_owner) == -EPERM);
    return NULL;
}

static char *test_bundle_wrong_identity_rejected(void)
{
    mu_assert("prefix", bundle_status(k_bundle_prefix) == -EPERM);
    mu_assert("suffix", bundle_status(k_bundle_suffix) == -EPERM);
    mu_assert("dot wildcard", bundle_status(k_bundle_dot) == -EPERM);
    mu_assert("other workflow", bundle_status(k_bundle_other_workflow) == -EPERM);
    mu_assert("two names", bundle_status(k_bundle_two_names) == -EBADMSG);
    return NULL;
}

/* A bundle whose certificate field could be read two ways is refused: two
 * certificates, the proto field name (here), or a `\u` escape that spells a
 * key (next test). */
static char *test_bundle_ambiguous_certificate_rejected_with_buffer(char *doc)
{
    int n = snprintf(doc, TEST_MAX_BUNDLE, "{\"rawBytes\":\"AA==\",%s", k_bundle_release + 1);
    mu_assert("snprintf two", n > 0 && (size_t)n < TEST_MAX_BUNDLE);
    mu_assert("two certificates", bundle_status(doc) == -EBADMSG);
    n = snprintf(doc, TEST_MAX_BUNDLE, "{\"raw_bytes\":\"AA==\",%s", k_bundle_release + 1);
    mu_assert("snprintf proto", n > 0 && (size_t)n < TEST_MAX_BUNDLE);
    mu_assert("proto field name", bundle_status(doc) == -EBADMSG);
    return NULL;
}

static char *test_bundle_ambiguous_certificate_rejected(void)
{
    char *doc = calloc(TEST_MAX_BUNDLE, 1u);
    if (!doc)
        return "allocation failed";
    char *result = test_bundle_ambiguous_certificate_rejected_with_buffer(doc);
    free(doc);
    return result;
}

static char *test_bundle_escaped_or_missing_certificate_rejected_with_buffer(char *doc)
{
    const int n = snprintf(doc, TEST_MAX_BUNDLE, "{\"x\":\"\\u0041\",%s", k_bundle_release + 1);
    mu_assert("snprintf escape", n > 0 && (size_t)n < TEST_MAX_BUNDLE);
    mu_assert("unicode escape", bundle_status(doc) == -EBADMSG);
    mu_assert("no certificate", bundle_status("{\"verificationMaterial\":{}}") == -EBADMSG);
    mu_assert("NULL", vmaf_dnn_signer_check_bundle(NULL, 0u) == -EBADMSG);
    return NULL;
}

static char *test_bundle_escaped_or_missing_certificate_rejected(void)
{
    char *doc = calloc(TEST_MAX_BUNDLE, 1u);
    if (!doc)
        return "allocation failed";
    char *result = test_bundle_escaped_or_missing_certificate_rejected_with_buffer(doc);
    free(doc);
    return result;
}

static char *test_bundle_malformed_rejected(void)
{
    mu_assert("not base64", bundle_status("{\"rawBytes\":\"!!!!\"}") == -EBADMSG);
    mu_assert("bad length", bundle_status("{\"rawBytes\":\"AAA\"}") == -EBADMSG);
    mu_assert("not DER", bundle_status("{\"rawBytes\":\"AAAA\"}") == -EBADMSG);
    mu_assert("unterminated", bundle_status("{\"rawBytes\":\"AAAA") == -EBADMSG);
    mu_assert("no colon", bundle_status("{\"rawBytes\" \"AAAA\"}") == -EBADMSG);
    return NULL;
}

/* Every strict prefix of a valid certificate is malformed, never accepted:
 * the walk never reads past the bytes it was given. */
static char *test_certificate_truncations_rejected_with_buffer(char *doc)
{
    static const char key[] = "\"rawBytes\":\"";
    const char *b64 = strstr(k_bundle_master, key);
    mu_assert("fixture has rawBytes", b64 != NULL);
    b64 += sizeof(key) - 1u;
    const char *end = strchr(b64, '"');
    mu_assert("fixture base64 ends", end != NULL);
    const size_t full = (size_t)(end - b64);
    for (size_t cut = 4u; cut < full; cut += 4u) {
        const int n =
            snprintf(doc, TEST_SHORT_BUNDLE,
                     "{\"mediaType\":\"application/vnd.dev.sigstore.bundle.v0.3+json\",%s%.*s\"}",
                     key, (int)cut, b64);
        mu_assert("snprintf cut", n > 0 && (size_t)n < TEST_SHORT_BUNDLE);
        mu_assert("truncated certificate rejected", bundle_status(doc) != 0);
    }
    return NULL;
}

static char *test_certificate_truncations_rejected(void)
{
    char *doc = calloc(TEST_SHORT_BUNDLE, 1u);
    if (!doc)
        return "allocation failed";
    char *result = test_certificate_truncations_rejected_with_buffer(doc);
    free(doc);
    return result;
}

static int bundle_with_key_status_with_buffer(const char *key, char *doc)
{
    /* SAFETY: the release fixture is a nonempty NUL-terminated string. */
    int n = snprintf(doc, TEST_MAX_BUNDLE, "{\"%s\":{},%s", key, k_bundle_release + 1);
    if (n < 0 || (size_t)n >= TEST_MAX_BUNDLE)
        return 0;
    return bundle_status(doc);
}

static int bundle_with_key_status(const char *key)
{
    char *doc = calloc(TEST_MAX_BUNDLE, 1u);
    if (!doc)
        return 0;
    int result = bundle_with_key_status_with_buffer(key, doc);
    free(doc);
    return result;
}

static char *test_bundle_refuses_cert(void)
{
    mu_assert("alternate key refused", bundle_with_key_status("cert") == -EBADMSG);
    return NULL;
}

static char *test_bundle_refuses_base64_signature(void)
{
    mu_assert("alternate key refused", bundle_with_key_status("base64Signature") == -EBADMSG);
    return NULL;
}

static char *test_bundle_refuses_rekor_bundle(void)
{
    mu_assert("alternate key refused", bundle_with_key_status("rekorBundle") == -EBADMSG);
    return NULL;
}

static char *test_bundle_refuses_certificate_chain(void)
{
    mu_assert("alternate key refused", bundle_with_key_status("x509CertificateChain") == -EBADMSG);
    return NULL;
}

static char *test_bundle_refuses_public_key(void)
{
    mu_assert("alternate key refused", bundle_with_key_status("publicKey") == -EBADMSG);
    return NULL;
}

static char *test_bundle_requires_single_certificate_format_with_buffer(char *doc)
{
    /* SAFETY: the release fixture starts with an opening brace. */
    int n = snprintf(doc, TEST_MAX_BUNDLE,
                     "{\"cert\":\"other\",\"base64Signature\":\"x\","
                     "\"rekorBundle\":{},%s",
                     k_bundle_release + 1);
    mu_assert("fixture fits", n > 0 && (size_t)n < TEST_MAX_BUNDLE);
    mu_assert("only current certificate format", bundle_status(doc) == -EBADMSG);
    return NULL;
}

static char *test_bundle_requires_single_certificate_format(void)
{
    char *doc = calloc(TEST_MAX_BUNDLE, 1u);
    if (!doc)
        return "allocation failed";
    char *result = test_bundle_requires_single_certificate_format_with_buffer(doc);
    free(doc);
    return result;
}

static char *test_bundle_requires_media_type_with_buffer(char *doc)
{
    const char *rest = strstr(k_bundle_release, ",\"verificationMaterial\"");
    mu_assert("fixture material", rest != NULL);
    int n = snprintf(doc, TEST_MAX_BUNDLE, "{%s", rest + 1);
    mu_assert("fixture fits", n > 0 && (size_t)n < TEST_MAX_BUNDLE);
    mu_assert("missing media type", bundle_status(doc) == -EBADMSG);
    return NULL;
}

static char *test_bundle_requires_media_type(void)
{
    char *doc = calloc(TEST_MAX_BUNDLE, 1u);
    if (!doc)
        return "allocation failed";
    char *result = test_bundle_requires_media_type_with_buffer(doc);
    free(doc);
    return result;
}

static char *test_bundle_requires_sigstore_media_type_with_buffer(char *doc)
{
    const char *rest = strstr(k_bundle_release, ",\"verificationMaterial\"");
    mu_assert("fixture material", rest != NULL);
    int n = snprintf(doc, TEST_MAX_BUNDLE, "{\"mediaType\":\"application/json\"%s", rest);
    mu_assert("fixture fits", n > 0 && (size_t)n < TEST_MAX_BUNDLE);
    mu_assert("wrong media type", bundle_status(doc) == -EBADMSG);
    return NULL;
}

static char *test_bundle_requires_sigstore_media_type(void)
{
    char *doc = calloc(TEST_MAX_BUNDLE, 1u);
    if (!doc)
        return "allocation failed";
    char *result = test_bundle_requires_sigstore_media_type_with_buffer(doc);
    free(doc);
    return result;
}

static char *test_bundle_refuses_proto_x509_certificate_chain(void)
{
    mu_assert("x509_certificate_chain refused",
              bundle_with_key_status("x509_certificate_chain") == -EBADMSG);
    return NULL;
}

static char *test_bundle_refuses_proto_public_key(void)
{
    mu_assert("public_key refused", bundle_with_key_status("public_key") == -EBADMSG);
    return NULL;
}

static char *test_bundle_refuses_proto_base64_signature(void)
{
    mu_assert("base64_signature refused", bundle_with_key_status("base64_signature") == -EBADMSG);
    return NULL;
}

static char *test_bundle_refuses_proto_rekor_bundle(void)
{
    mu_assert("rekor_bundle refused", bundle_with_key_status("rekor_bundle") == -EBADMSG);
    return NULL;
}

static char *test_bundle_refuses_proto_media_type(void)
{
    mu_assert("media_type refused", bundle_with_key_status("media_type") == -EBADMSG);
    return NULL;
}

static char *test_bundle_refuses_proto_verification_material(void)
{
    mu_assert("verification_material refused",
              bundle_with_key_status("verification_material") == -EBADMSG);
    return NULL;
}

static char *test_bundle_refuses_proto_message_signature(void)
{
    mu_assert("message_signature refused", bundle_with_key_status("message_signature") == -EBADMSG);
    return NULL;
}

static char *test_bundle_refuses_proto_dsse_envelope(void)
{
    mu_assert("dsse_envelope refused", bundle_with_key_status("dsse_envelope") == -EBADMSG);
    return NULL;
}

static char *test_bundle_refuses_proto_tlog_entries(void)
{
    mu_assert("tlog_entries refused", bundle_with_key_status("tlog_entries") == -EBADMSG);
    return NULL;
}

static char *test_bundle_refuses_proto_timestamp_verification_data(void)
{
    mu_assert("timestamp_verification_data refused",
              bundle_with_key_status("timestamp_verification_data") == -EBADMSG);
    return NULL;
}

static int bundle_with_trailing_key_status_with_buffer(const char *key, char *doc)
{
    const size_t len = sizeof(k_bundle_release) - 1u;
    int n =
        snprintf(doc, TEST_MAX_BUNDLE, "%.*s,\"%s\":{}}", (int)(len - 1u), k_bundle_release, key);
    if (n < 0 || (size_t)n >= TEST_MAX_BUNDLE)
        return 0;
    return bundle_status(doc);
}

static int bundle_with_trailing_key_status(const char *key)
{
    char *doc = calloc(TEST_MAX_BUNDLE, 1u);
    if (!doc)
        return 0;
    int result = bundle_with_trailing_key_status_with_buffer(key, doc);
    free(doc);
    return result;
}

static char *test_bundle_requires_one_certificate(void)
{
    mu_assert("one certificate", bundle_with_trailing_key_status("certificate") == -EBADMSG);
    return NULL;
}

static char *test_bundle_requires_raw_bytes_first_with_buffer(char *doc)
{
    const char *raw = strstr(k_bundle_release, "\"rawBytes\"");
    mu_assert("fixture rawBytes", raw != NULL);
    int n = snprintf(doc, TEST_MAX_BUNDLE, "%.*s\"extra\":0,%s", (int)(raw - k_bundle_release),
                     k_bundle_release, raw);
    mu_assert("fixture fits", n > 0 && (size_t)n < TEST_MAX_BUNDLE);
    mu_assert("rawBytes first", bundle_status(doc) == -EBADMSG);
    return NULL;
}

static char *test_bundle_requires_raw_bytes_first(void)
{
    char *doc = calloc(TEST_MAX_BUNDLE, 1u);
    if (!doc)
        return "allocation failed";
    char *result = test_bundle_requires_raw_bytes_first_with_buffer(doc);
    free(doc);
    return result;
}

static char *test_bundle_requires_top_level_media_type_with_buffer(char *doc)
{
    const char *rest = strstr(k_bundle_release, ",\"verificationMaterial\"");
    mu_assert("fixture material", rest != NULL);
    int n = snprintf(doc, TEST_MAX_BUNDLE,
                     "{\"nested\":{\"mediaType\":"
                     "\"application/vnd.dev.sigstore.bundle.v0.3+json\"}%s",
                     rest);
    mu_assert("fixture fits", n > 0 && (size_t)n < TEST_MAX_BUNDLE);
    mu_assert("top-level mediaType", bundle_status(doc) == -EBADMSG);
    return NULL;
}

static char *test_bundle_requires_top_level_media_type(void)
{
    char *doc = calloc(TEST_MAX_BUNDLE, 1u);
    if (!doc)
        return "allocation failed";
    char *result = test_bundle_requires_top_level_media_type_with_buffer(doc);
    free(doc);
    return result;
}

static char *test_bundle_requires_one_media_type(void)
{
    mu_assert("one mediaType", bundle_with_trailing_key_status("mediaType") == -EBADMSG);
    return NULL;
}

static char *test_bundle_full_shape_accepted(void)
{
    mu_assert("full release bundle shape", bundle_status(k_bundle_full_shape) == 0);
    return NULL;
}

static char *test_certificate_rejects_duplicate_owner(void)
{
    mu_assert("duplicate_owner", bundle_status(k_bundle_duplicate_owner) != 0);
    return NULL;
}

static char *test_certificate_rejects_duplicate_san(void)
{
    mu_assert("duplicate_san", bundle_status(k_bundle_duplicate_san) != 0);
    return NULL;
}

static char *test_certificate_rejects_owner_long(void)
{
    mu_assert("owner_long", bundle_status(k_bundle_owner_long) != 0);
    return NULL;
}

static char *test_certificate_rejects_owner_short(void)
{
    mu_assert("owner_short", bundle_status(k_bundle_owner_short) != 0);
    return NULL;
}

static char *test_certificate_rejects_owner_empty(void)
{
    mu_assert("owner_empty", bundle_status(k_bundle_owner_empty) != 0);
    return NULL;
}

static char *test_certificate_rejects_owner_space(void)
{
    mu_assert("owner_space", bundle_status(k_bundle_owner_space) != 0);
    return NULL;
}

static char *test_certificate_rejects_owner_tag(void)
{
    mu_assert("owner_tag", bundle_status(k_bundle_owner_tag) != 0);
    return NULL;
}

static char *test_certificate_rejects_san_email(void)
{
    mu_assert("san_email", bundle_status(k_bundle_san_email) != 0);
    return NULL;
}

static char *test_certificate_rejects_san_dns(void)
{
    mu_assert("san_dns", bundle_status(k_bundle_san_dns) != 0);
    return NULL;
}

static char *test_certificate_rejects_san_nul(void)
{
    mu_assert("san_nul", bundle_status(k_bundle_san_nul) != 0);
    return NULL;
}

static char *test_certificate_rejects_nonminimal_length(void)
{
    mu_assert("nonminimal_length", bundle_status(k_bundle_nonminimal_length) != 0);
    return NULL;
}

static char *test_certificate_rejects_trailing_bytes(void)
{
    mu_assert("trailing_bytes", bundle_status(k_bundle_trailing_bytes) != 0);
    return NULL;
}

static char *test_certificate_rejects_oversize(void)
{
    mu_assert("oversize", vmaf_dnn_signer_check_certificate(k_certificate_oversize,
                                                            sizeof(k_certificate_oversize)) != 0);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_certificate_rejects_duplicate_owner),
        MU_TEST(test_certificate_rejects_duplicate_san),
        MU_TEST(test_certificate_rejects_owner_long),
        MU_TEST(test_certificate_rejects_owner_short),
        MU_TEST(test_certificate_rejects_owner_empty),
        MU_TEST(test_certificate_rejects_owner_space),
        MU_TEST(test_certificate_rejects_owner_tag),
        MU_TEST(test_certificate_rejects_san_email),
        MU_TEST(test_certificate_rejects_san_dns),
        MU_TEST(test_certificate_rejects_san_nul),
        MU_TEST(test_certificate_rejects_nonminimal_length),
        MU_TEST(test_certificate_rejects_trailing_bytes),
        MU_TEST(test_certificate_rejects_oversize),

        MU_TEST(test_identity_accepts_release_and_master),
        MU_TEST(test_identity_rejects_prefix_and_suffix),
        MU_TEST(test_identity_rejects_other_names),
        MU_TEST(test_bundle_release_certificate_accepted),
        MU_TEST(test_bundle_wrong_owner_rejected),
        MU_TEST(test_bundle_wrong_identity_rejected),
        MU_TEST(test_bundle_ambiguous_certificate_rejected),
        MU_TEST(test_bundle_escaped_or_missing_certificate_rejected),
        MU_TEST(test_bundle_malformed_rejected),
        MU_TEST(test_certificate_truncations_rejected),
        MU_TEST(test_bundle_refuses_cert),
        MU_TEST(test_bundle_refuses_base64_signature),
        MU_TEST(test_bundle_refuses_rekor_bundle),
        MU_TEST(test_bundle_refuses_certificate_chain),
        MU_TEST(test_bundle_refuses_public_key),
        MU_TEST(test_bundle_requires_single_certificate_format),
        MU_TEST(test_bundle_requires_media_type),
        MU_TEST(test_bundle_requires_sigstore_media_type),
        MU_TEST(test_bundle_refuses_proto_x509_certificate_chain),
        MU_TEST(test_bundle_refuses_proto_public_key),
        MU_TEST(test_bundle_refuses_proto_base64_signature),
        MU_TEST(test_bundle_refuses_proto_rekor_bundle),
        MU_TEST(test_bundle_refuses_proto_media_type),
        MU_TEST(test_bundle_refuses_proto_verification_material),
        MU_TEST(test_bundle_refuses_proto_message_signature),
        MU_TEST(test_bundle_refuses_proto_dsse_envelope),
        MU_TEST(test_bundle_refuses_proto_tlog_entries),
        MU_TEST(test_bundle_refuses_proto_timestamp_verification_data),
        MU_TEST(test_bundle_requires_one_certificate),
        MU_TEST(test_bundle_requires_raw_bytes_first),
        MU_TEST(test_bundle_requires_top_level_media_type),
        MU_TEST(test_bundle_requires_one_media_type),
        MU_TEST(test_bundle_full_shape_accepted),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
