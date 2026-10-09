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
#include <string.h>

#include "mu_table.h"
#include "signer_fixtures.h"
#include "signer_identity.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit (ADR-1138). */

static int san_allowed(const char *san)
{
    return vmaf_dnn_signer_identity_allowed(san, strlen(san));
}

static int bundle_status(const char *bundle)
{
    return vmaf_dnn_signer_check_bundle(bundle, strlen(bundle));
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
static char *test_bundle_ambiguous_certificate_rejected(void)
{
    char doc[16384];
    int n = snprintf(doc, sizeof(doc), "{\"rawBytes\":\"AA==\",%s", k_bundle_release + 1);
    mu_assert("snprintf two", n > 0 && (size_t)n < sizeof(doc));
    mu_assert("two certificates", bundle_status(doc) == -EBADMSG);
    n = snprintf(doc, sizeof(doc), "{\"raw_bytes\":\"AA==\",%s", k_bundle_release + 1);
    mu_assert("snprintf proto", n > 0 && (size_t)n < sizeof(doc));
    mu_assert("proto field name", bundle_status(doc) == -EBADMSG);
    return NULL;
}

static char *test_bundle_escaped_or_missing_certificate_rejected(void)
{
    char doc[16384];
    const int n = snprintf(doc, sizeof(doc), "{\"x\":\"\\u0041\",%s", k_bundle_release + 1);
    mu_assert("snprintf escape", n > 0 && (size_t)n < sizeof(doc));
    mu_assert("unicode escape", bundle_status(doc) == -EBADMSG);
    mu_assert("no certificate", bundle_status("{\"verificationMaterial\":{}}") == -EBADMSG);
    mu_assert("NULL", vmaf_dnn_signer_check_bundle(NULL, 0u) == -EBADMSG);
    return NULL;
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
static char *test_certificate_truncations_rejected(void)
{
    static const char key[] = "\"rawBytes\":\"";
    const char *b64 = strstr(k_bundle_master, key);
    mu_assert("fixture has rawBytes", b64 != NULL);
    b64 += sizeof(key) - 1u;
    const char *end = strchr(b64, '"');
    mu_assert("fixture base64 ends", end != NULL);
    const size_t full = (size_t)(end - b64);
    char doc[8192];
    for (size_t cut = 4u; cut < full; cut += 4u) {
        const int n = snprintf(doc, sizeof(doc), "{%s%.*s\"}", key, (int)cut, b64);
        mu_assert("snprintf cut", n > 0 && (size_t)n < sizeof(doc));
        mu_assert("truncated certificate rejected", bundle_status(doc) != 0);
    }
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
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
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
