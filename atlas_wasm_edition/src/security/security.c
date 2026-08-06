/**
 * security.c - SHA-256 integrity and report helpers.
 *
 * The hash implementation is self-contained so the compiler does not need
 * OpenSSL or platform-specific crypto libraries.
 */

#include "security.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    unsigned char buf[64];
    int buf_len;
} Sha256Ctx;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(e, f, g) (((e) & (f)) ^ (~(e) & (g)))
#define MAJ(a, b, c) (((a) & (b)) ^ ((a) & (c)) ^ ((b) & (c)))
#define EP0(a) (ROR32((a), 2) ^ ROR32((a), 13) ^ ROR32((a), 22))
#define EP1(e) (ROR32((e), 6) ^ ROR32((e), 11) ^ ROR32((e), 25))
#define SIG0(x) (ROR32((x), 7) ^ ROR32((x), 18) ^ ((x) >> 3))
#define SIG1(x) (ROR32((x), 17) ^ ROR32((x), 19) ^ ((x) >> 10))

static void sha256_init(Sha256Ctx *ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bit_count = 0;
    ctx->buf_len = 0;
}

static void sha256_transform(Sha256Ctx *ctx, const unsigned char *block) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t t1, t2;

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++)
        w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K256[i] + w[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_update(Sha256Ctx *ctx, const unsigned char *data, size_t len) {
    ctx->bit_count += (uint64_t)len * 8u;
    for (size_t i = 0; i < len; i++) {
        ctx->buf[ctx->buf_len++] = data[i];
        if (ctx->buf_len == 64) {
            sha256_transform(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

static void sha256_final(Sha256Ctx *ctx, unsigned char digest[32]) {
    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 56) {
        while (ctx->buf_len < 64) ctx->buf[ctx->buf_len++] = 0;
        sha256_transform(ctx, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 56) ctx->buf[ctx->buf_len++] = 0;

    uint64_t bits = ctx->bit_count;
    for (int i = 7; i >= 0; i--) {
        ctx->buf[56 + i] = (unsigned char)(bits & 0xffu);
        bits >>= 8;
    }
    sha256_transform(ctx, ctx->buf);

    for (int i = 0; i < 8; i++) {
        digest[i * 4] = (unsigned char)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (unsigned char)ctx->state[i];
    }
}

static void bytes_to_hex(const unsigned char *bytes, int n,
                         char hex_out[SEC_SHA256_HEX_LEN]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        hex_out[i * 2] = hex[bytes[i] >> 4];
        hex_out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    hex_out[n * 2] = '\0';
}

void sec_sha256_bytes(const unsigned char *data, size_t len,
                      char hex_out[SEC_SHA256_HEX_LEN]) {
    unsigned char digest[32];
    Sha256Ctx ctx;
    if (!hex_out) return;
    sha256_init(&ctx);
    if (data && len > 0) sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
    bytes_to_hex(digest, 32, hex_out);
}

void sec_sha256_string(const char *str, char hex_out[SEC_SHA256_HEX_LEN]) {
    sec_sha256_bytes((const unsigned char *)(str ? str : ""),
                     str ? strlen(str) : 0, hex_out);
}

int sec_sha256_file(const char *path, char hex_out[SEC_SHA256_HEX_LEN]) {
    FILE *f;
    unsigned char buf[8192];
    size_t n;
    unsigned char digest[32];
    Sha256Ctx ctx;

    if (!path || !hex_out) return 0;
    hex_out[0] = '\0';
    f = fopen(path, "rb");
    if (!f) return 0;

    sha256_init(&ctx);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha256_update(&ctx, buf, n);

    if (ferror(f)) {
        fclose(f);
        return 0;
    }
    fclose(f);

    sha256_final(&ctx, digest);
    bytes_to_hex(digest, 32, hex_out);
    return 1;
}

int sec_is_sha256_hex(const char *hex) {
    if (!hex || strlen(hex) != 64) return 0;
    for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)hex[i])) return 0;
    return 1;
}

int sec_hash_equal(const char *a, const char *b) {
    unsigned char diff = 0;
    if (!sec_is_sha256_hex(a) || !sec_is_sha256_hex(b)) return 0;
    for (int i = 0; i < 64; i++)
        diff |= (unsigned char)(tolower((unsigned char)a[i]) -
                                tolower((unsigned char)b[i]));
    return diff == 0;
}

static void json_escape(FILE *f, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        switch (*p) {
            case '\"': fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default:
                if (*p < 0x20) fprintf(f, "\\u%04x", *p);
                else fputc(*p, f);
                break;
        }
    }
}

int sec_write_integrity_report(const char *path,
                               const char *src_path,
                               const char *out_path,
                               const char *source_hash,
                               const char *output_hash,
                               int target,
                               int format,
                               const char *cpu_arch,
                               int hardening_level,
                               int deterministic,
                               int success,
                               int symbols,
                               int ast_nodes,
                               int safety_sil,
                               int quality_score) {
    FILE *f;
    time_t now;
    char ts[32];
    struct tm *tm_info;

    if (!path) return 0;
    f = fopen(path, "w");
    if (!f) return 0;

    now = time(NULL);
    tm_info = gmtime(&now);
    if (tm_info)
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm_info);
    else
        strncpy(ts, "unknown", sizeof(ts));
    ts[sizeof(ts) - 1] = '\0';

    fprintf(f, "{\n");
    fprintf(f, "  \"compiler\": \"plc_compiler\",\n");
    fprintf(f, "  \"timestamp\": \"%s\",\n", ts);
    fprintf(f, "  \"success\": %s,\n", success ? "true" : "false");
    fprintf(f, "  \"source\": \"");
    json_escape(f, src_path ? src_path : "");
    fprintf(f, "\",\n");
    fprintf(f, "  \"output\": \"");
    json_escape(f, out_path ? out_path : "");
    fprintf(f, "\",\n");
    fprintf(f, "  \"source_sha256\": \"%s\",\n", source_hash ? source_hash : "");
    fprintf(f, "  \"output_sha256\": \"%s\",\n", output_hash ? output_hash : "");
    fprintf(f, "  \"target\": %d,\n", target);
    fprintf(f, "  \"format\": %d,\n", format);
    fprintf(f, "  \"cpu_arch\": \"");
    json_escape(f, cpu_arch ? cpu_arch : "");
    fprintf(f, "\",\n");
    fprintf(f, "  \"hardening_level\": %d,\n", hardening_level);
    fprintf(f, "  \"deterministic_output\": %s,\n", deterministic ? "true" : "false");
    fprintf(f, "  \"symbols\": %d,\n", symbols);
    fprintf(f, "  \"ast_nodes\": %d,\n", ast_nodes);
    fprintf(f, "  \"safety_sil\": %d,\n", safety_sil);
    fprintf(f, "  \"quality_score\": %d,\n", quality_score);
    fprintf(f, "  \"hardening_controls\": [\n");
    fprintf(f, "    \"source_sha256\", \"output_sha256\", \"expected_source_hash_gate\",\n");
    fprintf(f, "    \"constant_time_hash_compare\", \"bounded_source_read\", \"binary_source_reject\",\n");
    fprintf(f, "    \"bounded_tokens\", \"bounded_ast_nodes\", \"bounded_symbols\",\n");
    fprintf(f, "    \"safe_numeric_parse\", \"path_control_character_reject\", \"sandbox_path_mode\",\n");
    fprintf(f, "    \"parent_traversal_reject\", \"absolute_path_reject\", \"symlink_output_reject\",\n");
    fprintf(f, "    \"atomic_temp_write\", \"overwrite_refusal\", \"write_error_checking\",\n");
    fprintf(f, "    \"deterministic_timestamps\", \"cpu_profile_manifest\", \"strict_warning_gate\",\n");
    fprintf(f, "    \"safety_critical_gate\", \"minimum_sil_gate\", \"required_estop_gate\",\n");
    fprintf(f, "    \"required_else_gate\", \"diagnostics_quality_score\", \"iec61508_safety_report\",\n");
    fprintf(f, "    \"resource_limit_manifest\", \"vendor_target_manifest\", \"machine_readable_json\"\n");
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
    return 1;
}

void sec_memzero(void *ptr, size_t len) {
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    if (!ptr) return;
    while (len--) *p++ = 0;
}
