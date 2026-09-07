/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Minimal ONNX-protobuf scanner. See onnx_scan.h for rationale.
 *
 *  All parsing is bounds-checked on every read; on malformed input we
 *  return -EBADMSG rather than deref into undefined memory. The scanner
 *  only descends three fixed levels (Model → Graph → Node → op_type
 *  string); it does not recurse into arbitrary nested messages.
 */

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "onnx_scan.h"
#include "op_allowlist.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Protobuf wire types (see developers.google.com/protocol-buffers/docs/encoding). */
enum {
    PB_WIRE_VARINT = 0,
    PB_WIRE_64BIT = 1,
    PB_WIRE_LEN = 2,
    PB_WIRE_32BIT = 5,
};

/* ONNX field numbers we care about. */
enum {
    MODEL_GRAPH_FIELD = 7,    /* ModelProto.graph (GraphProto) */
    GRAPH_NODE_FIELD = 1,     /* GraphProto.node (repeated NodeProto) */
    NODE_OP_TYPE_FIELD = 4,   /* NodeProto.op_type (string) */
    NODE_ATTRIBUTE_FIELD = 5, /* NodeProto.attribute (repeated AttributeProto) */
    /* NodeProto.domain (field 7, string): identifies the operator set.
     * Standard built-in ops use the empty string "" (equivalent to
     * "ai.onnx"). Any non-empty, non-"ai.onnx" domain means a custom
     * op set — rejected unconditionally regardless of op_type, because
     * the (domain, op_type) tuple determines ORT dispatch, so an
     * adversarial model could shadow an allowlisted name with a
     * custom-domain op that executes arbitrary code.
     * ADR-1089 (fix/dnn-onnx-domain-bypass). */
    NODE_DOMAIN_FIELD = 7,
    /* AttributeProto fields with embedded subgraphs — the bodies of
     * control-flow ops (Loop.body, If.then_branch, If.else_branch).
     * Recursive scan covers ADR-0169 / T6-5: forbidden ops cannot hide
     * inside a control-flow subgraph. */
    ATTR_G_FIELD = 6,       /* AttributeProto.g (single GraphProto) */
    ATTR_GRAPHS_FIELD = 11, /* AttributeProto.graphs (repeated GraphProto) */
};

/* Maximum nested subgraph depth permitted. Real models keep nesting
 * shallow (Loop within If at most). The cap is a defence-in-depth bound
 * against pathological recursion in malformed input. */
#define VMAF_DNN_MAX_SUBGRAPH_DEPTH 8

/* Maximum number of `Loop` nodes permitted across the whole ModelProto
 * (top-level graph + every embedded subgraph). ADR-0171 / T6-5b: the
 * wire-format scanner can't trace `Loop.M` data flow back to a Constant
 * (that's the export-time Python check's job), so we bound the worst-
 * case "thousands of nested Loops within Loops" attack purely by node
 * count. Real models — diffusion, RAFT, MUSIQ — fit comfortably under
 * 16 Loop nodes. */
#define VMAF_DNN_MAX_LOOP_NODES 16

/* Varint max length for uint64 is 10 bytes (64 bits / 7 bits-per-byte, rounded up). */
#define PB_VARINT_MAX_BYTES 10

/* Read a varint from buf[*off..len). Advance *off past the varint on
 * success. Returns 0 on success, -EBADMSG on truncated / overlong varint. */
static int pb_read_varint(const unsigned char *buf, size_t len, size_t *off, uint64_t *out)
{
    assert(buf != NULL);
    assert(off != NULL);
    assert(out != NULL);

    uint64_t value = 0;
    unsigned shift = 0;
    size_t i = *off;
    size_t consumed = 0;

    while (consumed < PB_VARINT_MAX_BYTES) {
        if (i >= len) {
            return -EBADMSG;
        }
        const unsigned char byte = buf[i++];
        consumed++;
        value |= (uint64_t)(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0u) {
            *off = i;
            *out = value;
            return 0;
        }
        shift += 7;
        if (shift >= 64u) {
            return -EBADMSG;
        }
    }
    /* Ran past PB_VARINT_MAX_BYTES without hitting a terminator. */
    return -EBADMSG;
}

/* Skip the value portion of a field whose tag we already consumed, given
 * the wire type. Advances *off. Returns 0 or -EBADMSG. */
static int pb_skip_field(const unsigned char *buf, size_t len, size_t *off, unsigned wire_type)
{
    assert(buf != NULL);
    assert(off != NULL);

    switch (wire_type) {
    case PB_WIRE_VARINT: {
        uint64_t scratch = 0;
        return pb_read_varint(buf, len, off, &scratch);
    }
    case PB_WIRE_64BIT: {
        if (*off + 8u > len) {
            return -EBADMSG;
        }
        *off += 8u;
        return 0;
    }
    case PB_WIRE_LEN: {
        uint64_t sub_len = 0;
        const int err = pb_read_varint(buf, len, off, &sub_len);
        if (err != 0) {
            return err;
        }
        if (sub_len > (uint64_t)(len - *off)) {
            return -EBADMSG;
        }
        *off += (size_t)sub_len;
        return 0;
    }
    case PB_WIRE_32BIT: {
        if (*off + 4u > len) {
            return -EBADMSG;
        }
        *off += 4u;
        return 0;
    }
    default:
        /* Groups (wire types 3, 4) are deprecated in proto3 and not emitted
         * by ONNX exporters. Reject rather than attempt to parse them. */
        return -EBADMSG;
    }
}

/* Check one op_type string extracted from a NodeProto. Returns 0 if the
 * op is allowed, -EPERM otherwise. On first rejection populates *first_bad. */
static int check_op_name(const char *op_name, size_t slen, char **first_bad)
{
    if (vmaf_dnn_op_allowed(op_name)) {
        return 0;
    }
    if (first_bad && *first_bad == NULL) {
        char *copy = (char *)malloc(slen + 1u);
        if (!copy) {
            return -ENOMEM;
        }
        memcpy(copy, op_name, slen + 1u);
        *first_bad = copy;
    }
    return -EPERM;
}

static int check_loop_budget(const char *op_name, size_t slen, unsigned *loop_count,
                             char **first_bad)
{
    if (!loop_count || strcmp(op_name, "Loop") != 0) {
        return 0;
    }
    if (++(*loop_count) <= VMAF_DNN_MAX_LOOP_NODES) {
        return 0;
    }
    if (first_bad && *first_bad == NULL) {
        /* Surface the rejection through the same channel as a
         * forbidden op — callers already log first_bad. */
        char *copy = (char *)malloc(slen + 1u);
        if (!copy) {
            return -ENOMEM;
        }
        memcpy(copy, op_name, slen + 1u);
        *first_bad = copy;
    }
    return -EPERM;
}

/* Read the op_type string from a NodeProto field whose tag was already
 * consumed. Advances *off past the string on success. Increments
 * *loop_count when the op is "Loop" (ADR-0171 / T6-5b). */
static int read_op_type(const unsigned char *buf, size_t len, size_t *off, char **first_bad,
                        unsigned *loop_count)
{
    uint64_t slen = 0;
    int err = pb_read_varint(buf, len, off, &slen);
    if (err != 0) {
        return err;
    }
    if (slen > (uint64_t)(len - *off)) {
        return -EBADMSG;
    }
    /* ONNX op names are short (longest in ONNX opset is ~40 chars). Reject
     * pathologically long names; they aren't a real op we'd allow anyway. */
    if (slen == 0u || slen > 127u) {
        return -EPERM;
    }
    char op_name[128];
    memcpy(op_name, buf + *off, (size_t)slen);
    op_name[slen] = '\0';
    *off += (size_t)slen;
    err = check_op_name(op_name, (size_t)slen, first_bad);
    if (err != 0) {
        return err;
    }
    return check_loop_budget(op_name, (size_t)slen, loop_count, first_bad);
}

/* Check a NodeProto.domain string extracted from a node field whose tag was
 * already consumed.  Advances *off past the string.  The only permitted
 * domain values are the empty string "" (standard ONNX built-in ops) and
 * "ai.onnx" (explicit standard domain).  Any other domain — including every
 * third-party / custom domain — is rejected with -EPERM because ORT
 * dispatches via (domain, op_type) and a non-standard domain can shadow an
 * allowlisted op_type name with arbitrary custom-op code.
 * ADR-1089 / fix/dnn-onnx-domain-bypass. */
static int read_domain(const unsigned char *buf, size_t len, size_t *off, char **first_bad)
{
    uint64_t slen = 0;
    const int err = pb_read_varint(buf, len, off, &slen);
    if (err != 0) {
        return err;
    }
    if (slen > (uint64_t)(len - *off)) {
        return -EBADMSG;
    }
    /* Empty-string domain == standard "ai.onnx" domain: always allowed. */
    if (slen == 0u) {
        return 0;
    }
    /* Reject pathologically long names before touching them. */
    if (slen > 127u) {
        if (first_bad && *first_bad == NULL) {
            char *copy = (char *)malloc(sizeof("<custom-domain>"));
            if (copy) {
                memcpy(copy, "<custom-domain>", sizeof("<custom-domain>"));
                *first_bad = copy;
            }
        }
        *off += (size_t)slen;
        return -EPERM;
    }
    char domain[128];
    memcpy(domain, buf + *off, (size_t)slen);
    domain[slen] = '\0';
    *off += (size_t)slen;
    /* "ai.onnx" is the explicit standard domain name for built-in ops. */
    if (strcmp(domain, "ai.onnx") == 0) {
        return 0;
    }
    /* Any other domain is a custom / third-party op set: reject. */
    if (first_bad && *first_bad == NULL) {
        char *copy = (char *)malloc(slen + 1u);
        if (!copy) {
            return -ENOMEM;
        }
        memcpy(copy, domain, slen + 1u);
        *first_bad = copy;
    }
    return -EPERM;
}

/* Forward decl — scan_node and scan_attribute mutually recurse via
 * scan_graph for control-flow op subgraphs (Loop.body, If.then_branch,
 * If.else_branch). The shared `loop_count` counter (ADR-0171 / T6-5b)
 * threads through every level so a nested Loop tree counts toward the
 * top-level VMAF_DNN_MAX_LOOP_NODES cap. */
static int scan_graph(const unsigned char *buf, size_t len, char **first_bad, unsigned depth,
                      unsigned *loop_count);

/* Scan a single AttributeProto. We only care about embedded GraphProto
 * fields (`g` for Loop.body / If.then_branch / If.else_branch; `graphs`
 * for the rare repeated-graph case). Everything else is skipped. */
static int scan_attribute(const unsigned char *buf, size_t len, char **first_bad, unsigned depth,
                          unsigned *loop_count)
{
    size_t off = 0;
    while (off < len) {
        uint64_t tag = 0;
        int err = pb_read_varint(buf, len, &off, &tag);
        if (err != 0) {
            return err;
        }
        const unsigned field = (unsigned)(tag >> 3);
        const unsigned wire = (unsigned)(tag & 0x7u);

        if ((field == ATTR_G_FIELD || field == ATTR_GRAPHS_FIELD) && wire == PB_WIRE_LEN) {
            uint64_t glen = 0;
            err = pb_read_varint(buf, len, &off, &glen);
            if (err != 0) {
                return err;
            }
            if (glen > (uint64_t)(len - off)) {
                return -EBADMSG;
            }
            err = scan_graph(buf + off, (size_t)glen, first_bad, depth + 1u, loop_count);
            if (err != 0) {
                return err;
            }
            off += (size_t)glen;
        } else {
            err = pb_skip_field(buf, len, &off, wire);
            if (err != 0) {
                return err;
            }
        }
    }
    return 0;
}

/* Scan one NodeProto. Validates the op_type against the allowlist and,
 * for ops with embedded subgraphs (Loop / If), recursively scans each
 * subgraph so forbidden ops can't hide inside a control-flow body. */
static int scan_node(const unsigned char *buf, size_t len, char **first_bad, unsigned depth,
                     unsigned *loop_count)
{
    size_t off = 0;
    while (off < len) {
        uint64_t tag = 0;
        int err = pb_read_varint(buf, len, &off, &tag);
        if (err != 0) {
            return err;
        }
        const unsigned field = (unsigned)(tag >> 3);
        const unsigned wire = (unsigned)(tag & 0x7u);

        if (field == NODE_OP_TYPE_FIELD && wire == PB_WIRE_LEN) {
            err = read_op_type(buf, len, &off, first_bad, loop_count);
        } else if (field == NODE_DOMAIN_FIELD && wire == PB_WIRE_LEN) {
            /* ADR-1089: reject any non-standard domain so a custom op
             * cannot shadow an allowlisted op_type name. */
            err = read_domain(buf, len, &off, first_bad);
        } else if (field == NODE_ATTRIBUTE_FIELD && wire == PB_WIRE_LEN) {
            uint64_t alen = 0;
            err = pb_read_varint(buf, len, &off, &alen);
            if (err != 0) {
                return err;
            }
            if (alen > (uint64_t)(len - off)) {
                return -EBADMSG;
            }
            err = scan_attribute(buf + off, (size_t)alen, first_bad, depth, loop_count);
            if (err != 0) {
                return err;
            }
            off += (size_t)alen;
        } else {
            err = pb_skip_field(buf, len, &off, wire);
        }
        if (err != 0) {
            return err;
        }
    }
    return 0;
}

/* Scan one GraphProto: iterate its NodeProto children. */
static int scan_graph(const unsigned char *buf, size_t len, char **first_bad, unsigned depth,
                      unsigned *loop_count)
{
    if (depth > VMAF_DNN_MAX_SUBGRAPH_DEPTH) {
        /* Defence-in-depth bound on control-flow nesting. */
        return -EBADMSG;
    }
    size_t off = 0;
    while (off < len) {
        uint64_t tag = 0;
        int err = pb_read_varint(buf, len, &off, &tag);
        if (err != 0) {
            return err;
        }
        const unsigned field = (unsigned)(tag >> 3);
        const unsigned wire = (unsigned)(tag & 0x7u);

        if (field == GRAPH_NODE_FIELD && wire == PB_WIRE_LEN) {
            uint64_t nlen = 0;
            err = pb_read_varint(buf, len, &off, &nlen);
            if (err != 0) {
                return err;
            }
            if (nlen > (uint64_t)(len - off)) {
                return -EBADMSG;
            }
            err = scan_node(buf + off, (size_t)nlen, first_bad, depth, loop_count);
            if (err != 0) {
                return err;
            }
            off += (size_t)nlen;
        } else {
            err = pb_skip_field(buf, len, &off, wire);
            if (err != 0) {
                return err;
            }
        }
    }
    return 0;
}

int vmaf_dnn_scan_onnx(const unsigned char *buf, size_t len, char **first_bad)
{
    if (!buf || len == 0u) {
        return -EINVAL;
    }
    assert(buf != NULL);
    assert(len > 0u);
    if (first_bad) {
        *first_bad = NULL;
    }

    size_t off = 0;
    int graph_found = 0;
    /* Top-level Loop-node counter. Threaded through scan_graph /
     * scan_node / scan_attribute so a nested Loop inside a Loop body
     * still increments the same counter (ADR-0171 / T6-5b). */
    unsigned loop_count = 0;
    while (off < len) {
        uint64_t tag = 0;
        int err = pb_read_varint(buf, len, &off, &tag);
        if (err != 0) {
            return err;
        }
        assert(off <= len);
        const unsigned field = (unsigned)(tag >> 3);
        const unsigned wire = (unsigned)(tag & 0x7u);

        if (field == MODEL_GRAPH_FIELD && wire == PB_WIRE_LEN) {
            uint64_t glen = 0;
            err = pb_read_varint(buf, len, &off, &glen);
            if (err != 0) {
                return err;
            }
            if (glen > (uint64_t)(len - off)) {
                return -EBADMSG;
            }
            assert(off + (size_t)glen <= len);
            err = scan_graph(buf + off, (size_t)glen, first_bad, 0u, &loop_count);
            if (err != 0) {
                return err;
            }
            graph_found = 1;
            off += (size_t)glen;
        } else {
            err = pb_skip_field(buf, len, &off, wire);
            if (err != 0) {
                return err;
            }
        }
    }
    assert(off <= len);
    return graph_found ? 0 : -ENOENT;
}
/* NOLINTEND(modernize-use-nullptr) */
