/**
 * atlas_wasm_bridge.c - Browser-facing API for the ATLAS PLC compiler.
 *
 * Build with Emscripten. The exported API returns heap-allocated JSON strings;
 * callers must release them with atlas_free().
 */

#include "plc_compiler.h"
#include "security.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define ATLAS_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define ATLAS_EXPORT
#endif

static char *read_text_or_empty(const char *path) {
    char *text = file_read(path);
    if (text) return text;
    text = (char *)calloc(1, 1);
    return text;
}

static void append_escaped(char **buf, size_t *len, size_t *cap, const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p) {
        char tmp[8];
        const char *out = tmp;
        if (*p == '"') strcpy(tmp, "\\\"");
        else if (*p == '\\') strcpy(tmp, "\\\\");
        else if (*p == '\n') strcpy(tmp, "\\n");
        else if (*p == '\r') strcpy(tmp, "\\r");
        else if (*p == '\t') strcpy(tmp, "\\t");
        else if (*p < 0x20) snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
        else { tmp[0] = (char)*p; tmp[1] = '\0'; }

        size_t n = strlen(out);
        if (*len + n + 1 > *cap) {
            *cap = (*cap + n + 1024) * 2;
            *buf = (char *)realloc(*buf, *cap);
            if (!*buf) return;
        }
        memcpy(*buf + *len, out, n);
        *len += n;
        (*buf)[*len] = '\0';
        p++;
    }
}

static int append_raw(char **buf, size_t *len, size_t *cap, const char *text) {
    const char *s = text ? text : "";
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        *cap = (*cap + n + 1024) * 2;
        *buf = (char *)realloc(*buf, *cap);
        if (!*buf) return 0;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 1;
}

static char *json_response(int ok, const char *st, const char *safety,
                           const char *diagnostics, const char *graph,
                           const char *error) {
    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)calloc(cap, 1);
    if (!buf) return NULL;

#define ADD_RAW(s) do { if (!append_raw(&buf, &len, &cap, (s))) return NULL; } while (0)
#define ADD_STR(s) append_escaped(&buf, &len, &cap, (s))

    ADD_RAW("{\"ok\":");
    ADD_RAW(ok ? "true" : "false");
    ADD_RAW(",\"edition\":\"");
    ADD_RAW("community-demo");
    ADD_RAW("\",\"st\":\"");
    ADD_STR(st);
    ADD_RAW("\",\"safety\":\"");
    ADD_STR(safety);
    ADD_RAW("\",\"diagnostics\":\"");
    ADD_STR(diagnostics);
    ADD_RAW("\",\"graph\":\"");
    ADD_STR(graph);
    ADD_RAW("\",\"error\":\"");
    ADD_STR(error);
    ADD_RAW("\"}");
    return buf;

#undef ADD_RAW
#undef ADD_STR
}

static int community_source_allowed(const char *source) {
    int lines = 1;
    size_t len = source ? strlen(source) : 0;
    if (len > 12000) return 0;
    for (const char *p = source; p && *p; p++)
        if (*p == '\n') lines++;
    return lines <= 80;
}

ATLAS_EXPORT
char *atlas_compile_demo(const char *source) {
    const char *src_path = "/tmp/atlas_input.dsl";
    const char *out_path = "/tmp/atlas_output.st";
    const char *safety_path = "/tmp/atlas_safety.txt";
    const char *diag_path = "/tmp/atlas_diag.json";
    const char *graph_path = "/tmp/atlas_logic.dot";
    CompilerCtx *ctx;
    char *st = NULL;
    char *safety = NULL;
    char *diag = NULL;
    char *graph = NULL;
    char *response = NULL;
    int ok;

    if (!source || !source[0])
        return json_response(0, "", "", "", "", "No DSL source provided.");

    if (!community_source_allowed(source)) {
        return json_response(0, "", "", "", "",
                             "Community demo supports up to 80 lines / 12 KB. Use the server API for production builds.");
    }

    if (!file_write(src_path, source))
        return json_response(0, "", "", "", "", "Failed to write source into WASM filesystem.");

    ctx = compiler_create(PLC_CODESYS, FMT_STRUCTURED_TEXT);
    if (!ctx)
        return json_response(0, "", "", "", "", "Failed to create compiler context.");

    ctx->quiet_mode = 1;
    ctx->print_generated_output = 0;
    ctx->deterministic_output = 1;
    ctx->run_safety = 1;
    ctx->run_diagnostics = 1;
    ctx->diagnostics_format = 1;

    strncpy(ctx->safety_report_path, safety_path, MAX_LOG_PATH - 1);
    strncpy(ctx->diagnostics_report_path, diag_path, MAX_LOG_PATH - 1);
    strncpy(ctx->graph_report_path, graph_path, MAX_LOG_PATH - 1);

    ok = compiler_compile(ctx, src_path, out_path);
    st = read_text_or_empty(out_path);
    safety = read_text_or_empty(safety_path);
    diag = read_text_or_empty(diag_path);
    graph = read_text_or_empty(graph_path);

    response = json_response(ok, st, safety, diag, graph,
                             ok ? "" : "Compilation failed. Check diagnostics.");

    free(st);
    free(safety);
    free(diag);
    free(graph);
    compiler_destroy(ctx);
    return response;
}

ATLAS_EXPORT
char *atlas_compile(const char *source, int premium) {
    (void)premium;
    return atlas_compile_demo(source);
}

ATLAS_EXPORT
char *atlas_sha256(const char *text) {
    char hash[SEC_SHA256_HEX_LEN];
    char *out = (char *)malloc(SEC_SHA256_HEX_LEN);
    if (!out) return NULL;
    sec_sha256_string(text ? text : "", hash);
    memcpy(out, hash, SEC_SHA256_HEX_LEN);
    return out;
}

ATLAS_EXPORT
void atlas_free(char *ptr) {
    free(ptr);
}
