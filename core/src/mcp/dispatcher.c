/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  JSON-RPC 2.0 dispatcher for the embedded MCP server. v1 routes:
 *      - `tools/list`          → list of available tools
 *      - `tools/call`          → invoke a tool by name
 *      - `resources/list`      → list of available resources (empty in v1)
 *      - `initialize`          → MCP handshake (returns serverInfo + caps)
 *
 *  Tool registry is static — Power-of-10 rule 3 (no dynamic alloc
 *  on dispatch-thread hot path beyond cJSON's internal buffers).
 */

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "3rdparty/cJSON/cJSON.h"
#include "feature/feature_extractor.h"
#include "mcp_internal.h"

/* Forward decls for the tool table. */
typedef int (*vmaf_mcp_tool_fn)(struct VmafMcpServer *server, const cJSON *arguments,
                                cJSON **result_out, char **error_message_out);

typedef struct VmafMcpToolEntry {
    const char *name;
    const char *description;
    const char *input_schema_json;
    vmaf_mcp_tool_fn fn;
} VmafMcpToolEntry;

/* ============================================================
 * Tool: list_features
 * ============================================================ */

extern VmafFeatureExtractor *vmaf_get_feature_extractor_by_name(const char *name);

/* The internal feature_extractor_list is file-static in
 * feature_extractor.c; we re-walk by iterating the public
 * vmaf_get_feature_extractor_by_name() lookup over a known set
 * is the wrong approach. Instead we expose a tiny accessor below
 * that mirrors how the test harness already enumerates the table.
 *
 * To avoid an ABI-visible accessor solely for MCP, we hard-code the
 * canonical fork-shipped list here and validate each entry against
 * the registry via vmaf_get_feature_extractor_by_name(). If a name
 * is unknown (e.g. extractor disabled at compile time) we skip it.
 * v2 will replace this with a public iterator.
 *
 * NOTE: lint justification (ADR-0209 / ADR-0278): this static table mirrors the
 * canonical extractor list in libvmaf/src/feature/feature_extractor.c.
 * Its drift cost is bounded — a missing extractor only shortens
 * the MCP listing; bit-exact correctness is unaffected.
 */
static const char *const k_feature_names[] = {
    "float_adm",   "float_vif",   "float_motion",   "float_ssim", "float_ms_ssim", "psnr", "ciede",
    "psnr_hvs",    "ssim",        "ms_ssim",        "vmaf",       "adm",           "vif",  "motion",
    "integer_adm", "integer_vif", "integer_motion", "cambi",      "psnr_hvs",      NULL,
};

static int tool_list_features(struct VmafMcpServer *server, const cJSON *arguments,
                              cJSON **result_out, char **error_message_out)
{
    (void)server;
    (void)arguments;
    (void)error_message_out;

    cJSON *result = cJSON_CreateObject();
    if (result == NULL)
        return -ENOMEM;
    cJSON *features = cJSON_AddArrayToObject(result, "features");
    if (features == NULL) {
        cJSON_Delete(result);
        return -ENOMEM;
    }

    unsigned added = 0u;
    for (unsigned i = 0u; i < VMAF_MCP_MAX_FEATURES && k_feature_names[i] != NULL; i++) {
        VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(k_feature_names[i]);
        if (fex == NULL)
            continue; /* extractor disabled at compile time. */
        cJSON *name = cJSON_CreateString(k_feature_names[i]);
        if (name == NULL) {
            cJSON_Delete(result);
            return -ENOMEM;
        }
        if (cJSON_AddItemToArray(features, name) == 0) {
            cJSON_Delete(name);
            cJSON_Delete(result);
            return -ENOMEM;
        }
        added++;
    }
    cJSON_AddNumberToObject(result, "count", (double)added);
    *result_out = result;
    return 0;
}

/* ============================================================
 * Tool: compute_vmaf (v1 placeholder)
 * ============================================================ */

static int tool_compute_vmaf(struct VmafMcpServer *server, const cJSON *arguments,
                             cJSON **result_out, char **error_message_out)
{
    (void)server;
    /* v2 (T5-2c): real scoring binding lives in compute_vmaf.c.
     * The cJSON pointers cross the internal-header boundary as
     * void* so mcp_internal.h does not have to drag in cJSON.h
     * for every TU. */
    void *result_void = NULL;
    int rc = vmaf_mcp_compute_vmaf((const void *)arguments, &result_void, error_message_out);
    if (rc != 0) {
        if (result_void != NULL)
            cJSON_Delete((cJSON *)result_void);
        return rc;
    }
    *result_out = (cJSON *)result_void;
    return 0;
}

/* ============================================================
 * Tool registry
 * ============================================================ */

static const VmafMcpToolEntry k_tool_table[] = {
    {
        .name = "list_features",
        .description = "List available libvmaf feature extractors compiled into this build.",
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
        .fn = tool_list_features,
    },
    {
        .name = "compute_vmaf",
        .description = "Compute pooled mean VMAF for a (reference, distorted) YUV420p pair. "
                       "Required: reference_path, distorted_path, width, height. Optional: "
                       "bitdepth (8, 10, 12, or 16; default 8), model_version "
                       "(default 'vmaf_v0.6.1'). Returns {score, frames_scored, bitdepth, "
                       "model_version, pool_method='mean'}.",
        .input_schema_json = "{\"type\":\"object\","
                             "\"properties\":{"
                             "\"reference_path\":{\"type\":\"string\"},"
                             "\"distorted_path\":{\"type\":\"string\"},"
                             "\"width\":{\"type\":\"integer\"},"
                             "\"height\":{\"type\":\"integer\"},"
                             "\"bitdepth\":{\"type\":\"integer\",\"enum\":[8,10,12,16]},"
                             "\"model_version\":{\"type\":\"string\"}"
                             "},"
                             "\"required\":[\"reference_path\",\"distorted_path\","
                             "\"width\",\"height\"]}",
        .fn = tool_compute_vmaf,
    },
};

static const size_t k_tool_count = sizeof(k_tool_table) / sizeof(k_tool_table[0]);

/* ============================================================
 * JSON-RPC envelope helpers
 * ============================================================ */

/* JSON-RPC 2.0 error codes. */
enum {
    JSONRPC_PARSE_ERROR = -32700,
    JSONRPC_INVALID_REQUEST = -32600,
    JSONRPC_METHOD_NOT_FOUND = -32601,
    JSONRPC_INVALID_PARAMS = -32602,
    JSONRPC_INTERNAL_ERROR = -32603,
};

/* Build a JSON-RPC error envelope as a heap string. id may be NULL
 * (returns null id per JSON-RPC 2.0 §5.1). On success returns 0
 * and *out points to a malloc'd NUL-terminated buffer. */
static int build_error_response(const cJSON *id, int code, const char *message, char **out)
{
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL)
        return -ENOMEM;
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id != NULL && !cJSON_IsNull(id)) {
        cJSON *id_dup = cJSON_Duplicate(id, 1);
        if (id_dup == NULL) {
            cJSON_Delete(resp);
            return -ENOMEM;
        }
        cJSON_AddItemToObject(resp, "id", id_dup);
    } else {
        cJSON_AddNullToObject(resp, "id");
    }
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    if (err == NULL) {
        cJSON_Delete(resp);
        return -ENOMEM;
    }
    cJSON_AddNumberToObject(err, "code", (double)code);
    cJSON_AddStringToObject(err, "message", message != NULL ? message : "");

    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (str == NULL)
        return -ENOMEM;
    *out = str;
    return 0;
}

static int build_success_response(const cJSON *id, cJSON *result_owned, char **out)
{
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        cJSON_Delete(result_owned);
        return -ENOMEM;
    }
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id != NULL && !cJSON_IsNull(id)) {
        cJSON *id_dup = cJSON_Duplicate(id, 1);
        if (id_dup == NULL) {
            cJSON_Delete(resp);
            cJSON_Delete(result_owned);
            return -ENOMEM;
        }
        cJSON_AddItemToObject(resp, "id", id_dup);
    } else {
        cJSON_AddNullToObject(resp, "id");
    }
    cJSON_AddItemToObject(resp, "result", result_owned);

    char *str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (str == NULL)
        return -ENOMEM;
    *out = str;
    return 0;
}

/* ============================================================
 * Method handlers
 * ============================================================ */

static int handle_initialize(struct VmafMcpServer *server, cJSON **result_out)
{
    cJSON *result = cJSON_CreateObject();
    if (result == NULL)
        return -ENOMEM;
    cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
    cJSON *server_info = cJSON_AddObjectToObject(result, "serverInfo");
    if (server_info == NULL) {
        cJSON_Delete(result);
        return -ENOMEM;
    }
    cJSON_AddStringToObject(server_info, "name",
                            server->user_agent_owned != NULL ? server->user_agent_owned :
                                                               "libvmaf-mcp");
    cJSON_AddStringToObject(server_info, "version", "1.0.0");
    cJSON *capabilities = cJSON_AddObjectToObject(result, "capabilities");
    if (capabilities == NULL) {
        cJSON_Delete(result);
        return -ENOMEM;
    }
    cJSON_AddObjectToObject(capabilities, "tools");
    cJSON_AddObjectToObject(capabilities, "resources");
    *result_out = result;
    return 0;
}

static int handle_tools_list(cJSON **result_out)
{
    cJSON *result = cJSON_CreateObject();
    if (result == NULL)
        return -ENOMEM;
    cJSON *tools = cJSON_AddArrayToObject(result, "tools");
    if (tools == NULL) {
        cJSON_Delete(result);
        return -ENOMEM;
    }
    for (size_t i = 0u; i < k_tool_count; i++) {
        cJSON *t = cJSON_CreateObject();
        if (t == NULL) {
            cJSON_Delete(result);
            return -ENOMEM;
        }
        cJSON_AddStringToObject(t, "name", k_tool_table[i].name);
        cJSON_AddStringToObject(t, "description", k_tool_table[i].description);
        cJSON *schema = cJSON_Parse(k_tool_table[i].input_schema_json);
        if (schema == NULL) {
            cJSON_Delete(t);
            cJSON_Delete(result);
            return -EINVAL;
        }
        cJSON_AddItemToObject(t, "inputSchema", schema);
        if (cJSON_AddItemToArray(tools, t) == 0) {
            cJSON_Delete(t);
            cJSON_Delete(result);
            return -ENOMEM;
        }
    }
    *result_out = result;
    return 0;
}

static int handle_resources_list(cJSON **result_out)
{
    cJSON *result = cJSON_CreateObject();
    if (result == NULL)
        return -ENOMEM;
    cJSON *resources = cJSON_AddArrayToObject(result, "resources");
    if (resources == NULL) {
        cJSON_Delete(result);
        return -ENOMEM;
    }
    /* v1 ships zero resources. v2 will surface model files. */
    *result_out = result;
    return 0;
}

/* Wrap a tool's raw JSON result in MCP's content envelope:
 *   {"content":[{"type":"text","text":"<stringified tool_result>"}],"isError":false}
 * Consumes `tool_result_owned` regardless of outcome. */
static int wrap_tool_content(cJSON *tool_result_owned, cJSON **result_out)
{
    char *tool_str = cJSON_PrintUnformatted(tool_result_owned);
    cJSON_Delete(tool_result_owned);
    if (tool_str == NULL)
        return -ENOMEM;

    cJSON *result = cJSON_CreateObject();
    cJSON *content = result != NULL ? cJSON_AddArrayToObject(result, "content") : NULL;
    cJSON *block = content != NULL ? cJSON_CreateObject() : NULL;
    if (block == NULL) {
        cJSON_Delete(result);
        free(tool_str);
        return -ENOMEM;
    }
    cJSON_AddStringToObject(block, "type", "text");
    cJSON_AddStringToObject(block, "text", tool_str);
    free(tool_str);
    if (cJSON_AddItemToArray(content, block) == 0) {
        cJSON_Delete(block);
        cJSON_Delete(result);
        return -ENOMEM;
    }
    cJSON_AddBoolToObject(result, "isError", 0);
    *result_out = result;
    return 0;
}

static int handle_tools_call(struct VmafMcpServer *server, const cJSON *params, cJSON **result_out,
                             int *jsonrpc_error_code, char **error_message_out)
{
    if (params == NULL || !cJSON_IsObject(params)) {
        *jsonrpc_error_code = JSONRPC_INVALID_PARAMS;
        return -EINVAL;
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!cJSON_IsString(name)) {
        *jsonrpc_error_code = JSONRPC_INVALID_PARAMS;
        return -EINVAL;
    }
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");

    for (size_t i = 0u; i < k_tool_count; i++) {
        if (strcmp(name->valuestring, k_tool_table[i].name) != 0)
            continue;
        cJSON *tool_result = NULL;
        int rc = k_tool_table[i].fn(server, arguments, &tool_result, error_message_out);
        if (rc != 0) {
            *jsonrpc_error_code = rc == -EINVAL ? JSONRPC_INVALID_PARAMS : JSONRPC_INTERNAL_ERROR;
            return rc;
        }
        int wrap_rc = wrap_tool_content(tool_result, result_out);
        if (wrap_rc != 0) {
            *jsonrpc_error_code = JSONRPC_INTERNAL_ERROR;
            return wrap_rc;
        }
        return 0;
    }
    *jsonrpc_error_code = JSONRPC_METHOD_NOT_FOUND;
    return -ENOENT;
}

/* ============================================================
 * Top-level dispatch entry point
 * ============================================================ */

/* Route a parsed JSON-RPC request to the right handler. Sets
 * `*err_code` to a JSON-RPC error code on failure; sets `*result`
 * (caller-owned) on success. Returns 0 on success, negative errno
 * otherwise. */
static int route_method(struct VmafMcpServer *server, const char *method, const cJSON *params,
                        cJSON **result, int *err_code, char **error_message_owned)
{
    int rc;
    if (strcmp(method, "initialize") == 0) {
        rc = handle_initialize(server, result);
        if (rc != 0)
            *err_code = JSONRPC_INTERNAL_ERROR;
    } else if (strcmp(method, "tools/list") == 0) {
        rc = handle_tools_list(result);
        if (rc != 0)
            *err_code = JSONRPC_INTERNAL_ERROR;
    } else if (strcmp(method, "tools/call") == 0) {
        rc = handle_tools_call(server, params, result, err_code, error_message_owned);
    } else if (strcmp(method, "resources/list") == 0) {
        rc = handle_resources_list(result);
        if (rc != 0)
            *err_code = JSONRPC_INTERNAL_ERROR;
    } else {
        rc = -ENOENT;
        *err_code = JSONRPC_METHOD_NOT_FOUND;
    }
    return rc;
}

int vmaf_mcp_dispatch(struct VmafMcpServer *server, const char *request_buf, char **response_out)
{
    if (server == NULL || request_buf == NULL || response_out == NULL)
        return -EINVAL;
    /* Power-of-10 §5: post-guard — all three pointers known
     * non-NULL beyond this point. */
    assert(server != NULL);
    assert(request_buf != NULL);
    assert(response_out != NULL);
    *response_out = NULL;

    cJSON *root = cJSON_Parse(request_buf);
    if (root == NULL) {
        return build_error_response(NULL, JSONRPC_PARSE_ERROR, "parse error", response_out);
    }
    /* Power-of-10 §5: cJSON_Parse returns either NULL (handled
     * above) or a non-NULL handle. */
    assert(root != NULL);

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");

    if (!cJSON_IsString(method)) {
        int rc = build_error_response(id, JSONRPC_INVALID_REQUEST, "missing or invalid 'method'",
                                      response_out);
        cJSON_Delete(root);
        return rc;
    }

    int is_notification = (id == NULL || cJSON_IsNull(id));
    cJSON *result = NULL;
    int err_code = 0;
    char *error_message_owned = NULL;
    int rc =
        route_method(server, method->valuestring, params, &result, &err_code, &error_message_owned);

    if (is_notification) {
        if (result != NULL)
            cJSON_Delete(result);
        free(error_message_owned);
        cJSON_Delete(root);
        *response_out = NULL;
        return 0;
    }

    int build_rc;
    if (rc != 0) {
        const char *msg = error_message_owned != NULL          ? error_message_owned :
                          err_code == JSONRPC_METHOD_NOT_FOUND ? "method not found" :
                          err_code == JSONRPC_INVALID_PARAMS   ? "invalid params" :
                                                                 "internal error";
        build_rc = build_error_response(id, err_code, msg, response_out);
    } else {
        build_rc = build_success_response(id, result, response_out);
    }
    free(error_message_owned);
    cJSON_Delete(root);
    return build_rc;
}
