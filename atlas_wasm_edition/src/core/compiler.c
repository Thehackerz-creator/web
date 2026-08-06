#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

/**
 * simulation.c — Pre-deployment logic simulator
 * Evaluates all IF conditions with test vectors and shows expected outputs
 */

#include "plc_compiler.h"
#include "diagnostics.h"
#include "security.h"
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

double safe_atof(const char *str) {
    char *end = NULL;
    double value;

    if (!str) return 0.0;
    if (strlen(str) > MAX_IDENTIFIER_LEN) return 0.0;
    errno = 0;
    value = strtod(str, &end);
    if (errno == ERANGE || end == str || (end && *end != '\0') || !isfinite(value))
        return 0.0;
    return value;
}

static CPUArch detect_host_cpu_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return CPU_ARCH_X86_64;
#elif defined(__i386__) || defined(_M_IX86)
    return CPU_ARCH_X86;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return CPU_ARCH_AARCH64;
#elif defined(__arm__) || defined(_M_ARM)
    return CPU_ARCH_ARMV7;
#elif defined(__riscv) && (__riscv_xlen == 64)
    return CPU_ARCH_RISCV64;
#elif defined(__riscv)
    return CPU_ARCH_RISCV32;
#elif defined(__powerpc64__) || defined(__ppc64__)
    return CPU_ARCH_PPC64;
#elif defined(__mips64)
    return CPU_ARCH_MIPS64;
#elif defined(__mips__)
    return CPU_ARCH_MIPS32;
#elif defined(__sparc__) && defined(__arch64__)
    return CPU_ARCH_SPARC64;
#else
    return CPU_ARCH_AUTO;
#endif
}

const char *cpu_arch_cli_name(CPUArch arch) {
    if (arch == CPU_ARCH_AUTO) arch = detect_host_cpu_arch();
    switch (arch) {
        case CPU_ARCH_X86:     return "x86";
        case CPU_ARCH_X86_64:  return "x86_64";
        case CPU_ARCH_ARMV7:   return "armv7";
        case CPU_ARCH_AARCH64: return "aarch64";
        case CPU_ARCH_RISCV32: return "riscv32";
        case CPU_ARCH_RISCV64: return "riscv64";
        case CPU_ARCH_PPC64:   return "ppc64";
        case CPU_ARCH_MIPS32:  return "mips32";
        case CPU_ARCH_MIPS64:  return "mips64";
        case CPU_ARCH_SPARC64: return "sparc64";
        case CPU_ARCH_WASM32:  return "wasm32";
        default:               return "auto";
    }
}

const char *cpu_arch_name(CPUArch arch) {
    if (arch == CPU_ARCH_AUTO) arch = detect_host_cpu_arch();
    switch (arch) {
        case CPU_ARCH_X86:     return "Intel/AMD x86 (32-bit)";
        case CPU_ARCH_X86_64:  return "Intel/AMD x86_64 (64-bit)";
        case CPU_ARCH_ARMV7:   return "ARMv7-A/R (32-bit)";
        case CPU_ARCH_AARCH64: return "ARM AArch64 (64-bit)";
        case CPU_ARCH_RISCV32: return "RISC-V RV32 (32-bit)";
        case CPU_ARCH_RISCV64: return "RISC-V RV64 (64-bit)";
        case CPU_ARCH_PPC64:   return "PowerPC64 (64-bit)";
        case CPU_ARCH_MIPS32:  return "MIPS32 (32-bit)";
        case CPU_ARCH_MIPS64:  return "MIPS64 (64-bit)";
        case CPU_ARCH_SPARC64: return "SPARC V9 (64-bit)";
        case CPU_ARCH_WASM32:  return "WebAssembly WASM32";
        default:               return "Auto-detected host CPU";
    }
}

int cpu_arch_bits(CPUArch arch) {
    if (arch == CPU_ARCH_AUTO) arch = detect_host_cpu_arch();
    switch (arch) {
        case CPU_ARCH_X86:
        case CPU_ARCH_ARMV7:
        case CPU_ARCH_RISCV32:
        case CPU_ARCH_MIPS32:
        case CPU_ARCH_WASM32:
            return 32;
        default:
            return 64;
    }
}

const char *cpu_arch_endian(CPUArch arch) {
    if (arch == CPU_ARCH_AUTO) arch = detect_host_cpu_arch();
    switch (arch) {
        case CPU_ARCH_PPC64:
        case CPU_ARCH_SPARC64:
            return "big";
        case CPU_ARCH_MIPS32:
        case CPU_ARCH_MIPS64:
            return "bi";
        default:
            return "little";
    }
}

int cpu_arch_parse(const char *name, CPUArch *arch) {
    if (!name || !arch) return 0;
    if (strcasecmp(name, "auto") == 0 || strcasecmp(name, "host") == 0)
        { *arch = detect_host_cpu_arch(); return 1; }
    if (strcasecmp(name, "x86") == 0 || strcasecmp(name, "i386") == 0 ||
        strcasecmp(name, "i686") == 0)
        { *arch = CPU_ARCH_X86; return 1; }
    if (strcasecmp(name, "x86_64") == 0 || strcasecmp(name, "amd64") == 0 ||
        strcasecmp(name, "x64") == 0)
        { *arch = CPU_ARCH_X86_64; return 1; }
    if (strcasecmp(name, "arm") == 0 || strcasecmp(name, "armv7") == 0 ||
        strcasecmp(name, "arm32") == 0)
        { *arch = CPU_ARCH_ARMV7; return 1; }
    if (strcasecmp(name, "aarch64") == 0 || strcasecmp(name, "arm64") == 0)
        { *arch = CPU_ARCH_AARCH64; return 1; }
    if (strcasecmp(name, "riscv32") == 0 || strcasecmp(name, "rv32") == 0)
        { *arch = CPU_ARCH_RISCV32; return 1; }
    if (strcasecmp(name, "riscv64") == 0 || strcasecmp(name, "rv64") == 0 ||
        strcasecmp(name, "risc-v") == 0 || strcasecmp(name, "riscv") == 0)
        { *arch = CPU_ARCH_RISCV64; return 1; }
    if (strcasecmp(name, "ppc64") == 0 || strcasecmp(name, "powerpc64") == 0)
        { *arch = CPU_ARCH_PPC64; return 1; }
    if (strcasecmp(name, "mips32") == 0)
        { *arch = CPU_ARCH_MIPS32; return 1; }
    if (strcasecmp(name, "mips64") == 0)
        { *arch = CPU_ARCH_MIPS64; return 1; }
    if (strcasecmp(name, "sparc64") == 0 || strcasecmp(name, "sparcv9") == 0)
        { *arch = CPU_ARCH_SPARC64; return 1; }
    if (strcasecmp(name, "wasm32") == 0 || strcasecmp(name, "webassembly") == 0)
        { *arch = CPU_ARCH_WASM32; return 1; }
    return 0;
}

void cpu_arch_print_supported(FILE *out) {
    static const CPUArch arches[] = {
        CPU_ARCH_X86_64, CPU_ARCH_X86, CPU_ARCH_AARCH64, CPU_ARCH_ARMV7,
        CPU_ARCH_RISCV64, CPU_ARCH_RISCV32, CPU_ARCH_PPC64, CPU_ARCH_MIPS64,
        CPU_ARCH_MIPS32, CPU_ARCH_SPARC64, CPU_ARCH_WASM32
    };
    FILE *f = out ? out : stdout;
    fprintf(f, "Supported CPU profiles:\n");
    for (size_t i = 0; i < sizeof(arches) / sizeof(arches[0]); i++) {
        CPUArch arch = arches[i];
        fprintf(f, "  %-8s  %2d-bit  endian=%-6s  %s\n",
                cpu_arch_cli_name(arch), cpu_arch_bits(arch),
                cpu_arch_endian(arch), cpu_arch_name(arch));
    }
}

#ifdef _WIN32
#include <windows.h>
static double get_time_ms(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart / (double)freq.QuadPart * 1000.0;
}
#else
#include <sys/time.h>
static double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}
#endif

typedef struct { char name[MAX_IDENTIFIER_LEN]; double value; } SimVar;

static SimVar *sim_vars = NULL;
static int     sim_var_count = 0;
static int     sim_var_cap = 0;

static int ensure_sim_cap(int needed) {
    if (needed <= sim_var_cap) return 1;
    int new_cap = sim_var_cap > 0 ? sim_var_cap : 1024;
    while (new_cap < needed) new_cap *= 2;
    SimVar *nv = (SimVar *)realloc(sim_vars, (size_t)new_cap * sizeof(SimVar));
    if (!nv) return 0;
    sim_vars = nv;
    sim_var_cap = new_cap;
    return 1;
}

static void json_escape(const char *in, char *out, size_t outlen) {
    size_t oi = 0;
    if (!outlen) return;
    out[0] = '\0';
    for (size_t i = 0; in && in[i] && oi + 1 < outlen; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
            case '\"': out[oi++] = '\\'; out[oi++] = '\"'; break;
            case '\\': out[oi++] = '\\'; out[oi++] = '\\'; break;
            case '\b': out[oi++] = '\\'; out[oi++] = 'b';  break;
            case '\f': out[oi++] = '\\'; out[oi++] = 'f';  break;
            case '\n': out[oi++] = '\\'; out[oi++] = 'n';  break;
            case '\r': out[oi++] = '\\'; out[oi++] = 'r';  break;
            case '\t': out[oi++] = '\\'; out[oi++] = 't';  break;
            default:
                if (c < 0x20) {
                    /* Control characters -> \u00XX */
                    if (oi + 6 < outlen) {
                        snprintf(out + oi, outlen - oi, "\\u%04x", c);
                        oi += 6;
                    }
                } else {
                    out[oi++] = (char)c;
                }
        }
    }
    out[oi] = '\0';
}

static void emit_json_diagnostics(CompilerCtx *ctx, int success) {
    char esc_msg[MAX_ERROR_MSG * 2];
    char esc_name[MAX_IDENTIFIER_LEN * 2];
    char esc_addr[MAX_IDENTIFIER_LEN * 2];
    printf("{");
    printf("\"ok\":%s", success ? "true" : "false");
    printf(",\"target\":%d", (int)ctx->target);
    printf(",\"format\":%d", (int)ctx->fmt);
    printf(",\"stats\":{");
    printf("\"tokens\":%d", ctx->token_count);
    printf(",\"ast_nodes\":%d", ctx->nodes_allocated);
    printf(",\"symbols\":%d", ctx->sym_count);
    printf(",\"timers\":%d", ctx->timer_count);
    printf(",\"warnings\":%d", ctx->warning_count);
    printf(",\"errors\":%d", ctx->error_count);
    printf("}");
    printf(",\"errors\":[");
    for (int i = 0; i < ctx->error_count; i++) {
        if (i) printf(",");
        json_escape(ctx->error_messages[i], esc_msg, sizeof(esc_msg));
        printf("{\"line\":%d,\"message\":\"%s\"}", ctx->error_lines[i], esc_msg);
    }
    printf("]");
    printf(",\"symbols\":[");
    for (int i = 0; i < ctx->sym_count; i++) {
        if (i) printf(",");
        Symbol *s = &ctx->symbols[i];
        json_escape(s->name, esc_name, sizeof(esc_name));
        json_escape(s->plc_address, esc_addr, sizeof(esc_addr));
        printf("{\"name\":\"%s\",\"kind\":%d,\"dir\":%d,\"address\":\"%s\"}",
               esc_name, (int)s->kind, (int)s->direction, esc_addr);
    }
    printf("]");
    printf("}\n");
}

static double sim_get(const char *name) {
    for (int i = 0; i < sim_var_count; i++)
        if (strcasecmp(sim_vars[i].name, name) == 0)
            return sim_vars[i].value;
    return 0.0;
}

static void sim_set(const char *name, double val) {
    for (int i = 0; i < sim_var_count; i++) {
        if (strcasecmp(sim_vars[i].name, name) == 0) {
            sim_vars[i].value = val;
            return;
        }
    }
    if (!ensure_sim_cap(sim_var_count + 1)) return;
    strncpy(sim_vars[sim_var_count].name, name, MAX_IDENTIFIER_LEN - 1);
    sim_vars[sim_var_count].name[MAX_IDENTIFIER_LEN - 1] = '\0';
    sim_vars[sim_var_count].value = val;
    sim_var_count++;
}

static double val_to_double(const char *val) {
    if (strcasecmp(val, "ON")  == 0) return 1.0;
    if (strcasecmp(val, "OFF") == 0) return 0.0;
    return safe_atof(val);
}

static int eval_condition(ASTNode *node) {
    if (!node) return 0;
    switch (node->type) {
        case NODE_COMPARISON: {
            double lhs = sim_get(node->var_name);
            double rhs = node->is_numeric ? node->numeric_val : val_to_double(node->value);
            switch (node->cmp_op) {
                case CMP_EQ:  return lhs == rhs;
                case CMP_NEQ: return lhs != rhs;
                case CMP_GTE: return lhs >= rhs;
                case CMP_LTE: return lhs <= rhs;
                case CMP_GT:  return lhs >  rhs;
                case CMP_LT:  return lhs <  rhs;
            }
            return 0;
        }
        case NODE_AND: return eval_condition(node->children[0]) && eval_condition(node->children[1]);
        case NODE_OR:  return eval_condition(node->children[0]) || eval_condition(node->children[1]);
        case NODE_NOT: return !eval_condition(node->children[0]);
        default: return 0;
    }
}

static void apply_actions(ASTNode *head) {
    for (ASTNode *act = head; act; act = act->next) {
        if (act->type != NODE_ACTION || !act->var_name[0]) continue;
        sim_set(act->var_name, val_to_double(act->value));
    }
}

void simulate_run(CompilerCtx *ctx) {
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║           SIMULATION RUN (Test Vector Set 1)         ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    sim_var_count = 0;

    /* Initialize all input variables with test values */
    for (int i = 0; i < ctx->sym_count; i++) {
        Symbol *s = &ctx->symbols[i];
        if (s->direction == IO_INPUT || s->direction == IO_MEMORY) {
            double tv = 0.0;
            /* Heuristic test values */
            if (strstr(s->name, "switch") || strstr(s->name, "button"))
                tv = 1.0;   /* ON */
            else if (strstr(s->name, "temp") || strstr(s->name, "temperature"))
                tv = 55.0;
            else if (strstr(s->name, "level"))
                tv = 15.0;
            else if (strstr(s->name, "light_sensor") || strstr(s->name, "sensor"))
                tv = 250.0;
            else if (strstr(s->name, "pressure"))
                tv = 3.5;
            sim_set(s->name, tv);
        }
    }

    /* Print initial state */
    printf("  Input values:\n");
    for (int i = 0; i < sim_var_count; i++)
        printf("    %-20s = %.2f\n", sim_vars[i].name, sim_vars[i].value);

    printf("\n  Executing logic...\n\n");

    /* Execute each IF statement */
    for (int i = 0; i < ctx->ast_root->child_count; i++) {
        ASTNode *stmt = ctx->ast_root->children[i];
        if (stmt->type != NODE_IF) continue;

        int result = eval_condition(stmt->children[0]);
        printf("  [Statement %d, line %d]\n", i + 1, stmt->line);

        if (stmt->has_timer)
            printf("    Timer: %.2f %s (simulated as elapsed)\n",
                   stmt->timer_value,
                   stmt->timer_unit == TIMER_SECONDS ? "sec" :
                   stmt->timer_unit == TIMER_MINUTES ? "min" : "ms");

        printf("    Condition: %s\n", result ? "TRUE ✓" : "FALSE ✗");
        printf("    Branch   : %s\n", result ? "THEN" : "ELSE");

        if (result && stmt->child_count > 1)
            apply_actions(stmt->children[1]->next);
        else if (!result && stmt->child_count > 2)
            apply_actions(stmt->children[2]->next);

        printf("\n");
    }

    printf("  Output state after simulation:\n");
    for (int i = 0; i < ctx->sym_count; i++) {
        Symbol *s = &ctx->symbols[i];
        if (s->direction == IO_OUTPUT) {
            double v = sim_get(s->name);
            printf("    %-20s [%s] = %s (%.0f)\n",
                   s->name, s->plc_address, v ? "ON " : "OFF", v);
        }
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   file_io.c — File reading and writing
   ═══════════════════════════════════════════════════════════════════════════ */

static int path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static int path_has_parent_component(const char *path) {
    const char *p = path;
    if (!path) return 0;

    while (*p) {
        const char *start = p;
        int len = 0;
        while (*p && *p != '/' && *p != '\\') {
            p++;
            len++;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') return 1;
        while (*p == '/' || *p == '\\') p++;
    }
    return 0;
}

static int path_is_absolute_portable(const char *path) {
    if (!path || !path[0]) return 0;
    if (path[0] == '/' || path[0] == '\\') return 1;
    if (isalpha((unsigned char)path[0]) && path[1] == ':') return 1;
    return 0;
}

static int validate_path_for_ctx(CompilerCtx *ctx, const char *path,
                                 const char *role, int for_output) {
    size_t len;

    if (!path || !path[0]) {
        if (ctx) log_error(ctx, 0, "Security: empty %s path", role);
        else fprintf(stderr, "[ERROR] Empty %s path\n", role);
        return 0;
    }

    len = strlen(path);
    if (len >= 4096) {
        if (ctx) log_error(ctx, 0, "Security: %s path is too long", role);
        else fprintf(stderr, "[ERROR] %s path is too long\n", role);
        return 0;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c < 0x20 || c == 0x7f) {
            if (ctx) log_error(ctx, 0, "Security: %s path contains a control character", role);
            else fprintf(stderr, "[ERROR] %s path contains a control character\n", role);
            return 0;
        }
    }

    if (ctx && ctx->sandbox_paths) {
        if (path_is_absolute_portable(path)) {
            log_error(ctx, 0, "Security: %s path must be relative in sandbox-paths mode: %s",
                      role, path);
            return 0;
        }
        if (path_has_parent_component(path)) {
            log_error(ctx, 0, "Security: %s path must not contain '..' in sandbox-paths mode: %s",
                      role, path);
            return 0;
        }
    }

#ifndef _WIN32
    if (ctx && for_output && ctx->reject_output_symlink) {
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
            log_error(ctx, 0, "Security: refusing to write through symlink output path '%s'", path);
            return 0;
        }
    }
#else
    (void)for_output;
#endif

    return 1;
}

static char *file_read_limited(const char *path, unsigned int max_bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[ERROR] Cannot open '%s'\n", path); return NULL; }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "[ERROR] Cannot seek '%s'\n", path);
        return NULL;
    }
    long size = ftell(f);
    rewind(f);

    if (size <= 0 || size > (long)max_bytes) {
        fclose(f);
        fprintf(stderr, "[ERROR] File '%s' size invalid (%ld)\n", path, size);
        return NULL;
    }

    char *buf = (char *)malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t nread = fread(buf, 1, size, f);
    if (nread != (size_t)size || memchr(buf, '\0', nread) != NULL) {
        free(buf);
        fclose(f);
        fprintf(stderr, "[ERROR] File '%s' is not a valid text DSL source\n", path);
        return NULL;
    }
    buf[nread] = '\0';
    fclose(f);
    return buf;
}

char *file_read(const char *path) {
    return file_read_limited(path, DEFAULT_MAX_SOURCE_BYTES);
}

int file_write(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[ERROR] Cannot write '%s'\n", path); return 0; }
    fputs(content, f);
    fclose(f);
    return 1;
}

int compiler_write_output(CompilerCtx *ctx, const char *path,
                          const char *content) {
    char *tmp_path;
    size_t path_len;
    FILE *f;
    int ok = 0;

    if (!ctx || !content) return 0;
    if (!validate_path_for_ctx(ctx, path, "output", 1)) return 0;

    if (!ctx->allow_output_overwrite && path_exists(path)) {
        log_error(ctx, 0, "Security: refusing to overwrite existing output '%s'", path);
        return 0;
    }

    path_len = strlen(path);
    tmp_path = (char *)malloc(path_len + 64);
    if (!tmp_path) {
        log_error(ctx, 0, "Security: out of memory preparing output path");
        return 0;
    }

    snprintf(tmp_path, path_len + 64, "%s.tmp.%ld", path, (long)getpid());
    f = fopen(tmp_path, "wb");
    if (!f) {
        log_error(ctx, 0, "Security: cannot write temporary output '%s'", tmp_path);
        free(tmp_path);
        return 0;
    }

    if (fputs(content, f) >= 0 && fflush(f) == 0 && ferror(f) == 0)
        ok = 1;

    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        log_error(ctx, 0, "Security: failed while writing temporary output '%s'", tmp_path);
        remove(tmp_path);
        free(tmp_path);
        return 0;
    }

#ifdef _WIN32
    if (ctx->allow_output_overwrite && path_exists(path))
        remove(path);
#endif

    if (rename(tmp_path, path) != 0) {
        log_error(ctx, 0, "Security: failed to commit output '%s'", path);
        remove(tmp_path);
        free(tmp_path);
        return 0;
    }

    free(tmp_path);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   compiler.c — Main compiler orchestrator
   ═══════════════════════════════════════════════════════════════════════════ */

CompilerCtx *compiler_create(PLCTarget target, OutputFormat fmt) {
    CompilerCtx *ctx = (CompilerCtx *)calloc(1, sizeof(CompilerCtx));
    if (!ctx) return NULL;
    ctx->target = target;
    ctx->fmt    = fmt;

    /* Production defaults (no noisy printing unless explicitly enabled). */
    ctx->json_mode = 0;
    ctx->quiet_mode = 0;
    ctx->dump_tokens = 0;
    ctx->dump_ast = 0;
    ctx->dump_symbols = 0;
    ctx->run_simulation = 0;
    ctx->print_generated_output = 1;
    ctx->deterministic_output = 0;
    ctx->allow_output_overwrite = 1;
    ctx->sandbox_paths = 0;
    ctx->reject_output_symlink = 1;
    ctx->fail_on_warnings = 0;
    ctx->fail_on_safety_critical = 0;
    ctx->require_else = 0;
    ctx->require_estop = 0;
    ctx->min_sil = 0;
    ctx->hardening_level = HARDENING_STANDARD;
    ctx->cpu_arch = detect_host_cpu_arch();
    ctx->max_source_bytes = DEFAULT_MAX_SOURCE_BYTES;
    ctx->max_tokens = DEFAULT_MAX_TOKENS;
    ctx->max_ast_nodes = DEFAULT_MAX_AST_NODES;
    ctx->max_symbols = DEFAULT_MAX_SYMBOLS;

    /* v2.0 defaults */
    ctx->run_safety = 0;
    ctx->run_optimizer = 0;
    ctx->run_diagnostics = 0;
    ctx->diagnostics_format = DIAG_FMT_TEXT;
    ctx->diagnostics_quality_score = -1;
    ctx->safety_report_path[0] = '\0';
    ctx->diagnostics_report_path[0] = '\0';
    ctx->security_report_path[0] = '\0';
    ctx->graph_report_path[0] = '\0';
    ctx->expected_source_hash[0] = '\0';
    ctx->source_hash[0] = '\0';
    ctx->output_hash[0] = '\0';
    ctx->compile_time_ms = 0.0;

    /* Dynamic buffers */
    ctx->token_cap = 0;
    ctx->tokens = NULL;
    ctx->sym_cap = 0;
    ctx->symbols = NULL;
    ctx->next_scope_id = 1;

    ctx->log_path[0] = '\0';
    ctx->log_file = NULL;

    /* Arena GC */
    ctx->use_arena_gc = 1;
    gc_arena_init(&ctx->arena, 1u << 20);
    return ctx;
}

void compiler_apply_hardening_profile(CompilerCtx *ctx, HardeningLevel level) {
    if (!ctx) return;
    ctx->hardening_level = level;

    if (level == HARDENING_STANDARD) {
        return;
    }

    ctx->run_safety = 1;
    ctx->run_diagnostics = 1;
    ctx->reject_output_symlink = 1;
    ctx->fail_on_safety_critical = 1;
    ctx->require_else = 1;
    ctx->max_source_bytes = 8u * 1024u * 1024u;
    ctx->max_tokens = 500000;
    ctx->max_ast_nodes = 150000;
    ctx->max_symbols = 32768;

    if (level == HARDENING_PARANOID) {
        ctx->deterministic_output = 1;
        ctx->allow_output_overwrite = 0;
        ctx->sandbox_paths = 1;
        ctx->fail_on_warnings = 1;
        ctx->require_estop = 1;
        ctx->min_sil = 3;
        ctx->run_optimizer = 1;
        ctx->max_source_bytes = 4u * 1024u * 1024u;
        ctx->max_tokens = 250000;
        ctx->max_ast_nodes = 100000;
        ctx->max_symbols = 8192;
    }
}

void compiler_destroy(CompilerCtx *ctx) {
    if (!ctx) return;

    /* Free AST */
    if (ctx->ast_root) {
        ast_free(ctx, ctx->ast_root);
        ctx->ast_root = NULL;
    }

    /* Free source buffer if it was heap-allocated */
    /* (source pointer is borrowed; caller owns it) */

    log_info(ctx, "Memory: allocated=%d nodes, freed=%d nodes",
             ctx->nodes_allocated, ctx->nodes_freed);

    if (ctx->nodes_allocated != ctx->nodes_freed &&
        !(ctx && ctx->use_arena_gc))
        log_warn(ctx, "Memory: possible leak — %d nodes unfreed",
                 ctx->nodes_allocated - ctx->nodes_freed);

    log_close(ctx);
    gc_arena_destroy(&ctx->arena);
    free(ctx->tokens);
    free(ctx->symbols);
    free(ctx);
}

int compiler_compile(CompilerCtx *ctx, const char *src_path, const char *out_path) {
    double t_start = get_time_ms();
    const char *log_target = ctx->log_path[0] ? ctx->log_path : "plc_compiler.log";

    if (!ctx->json_mode && !ctx->quiet_mode) print_banner();
    if (!validate_path_for_ctx(ctx, log_target, "log", 1)) {
        if (ctx->json_mode) emit_json_diagnostics(ctx, 0);
        return 0;
    }
    log_init(ctx, log_target);

    log_info(ctx, "=== Compilation Start (v2.0) ===");
    log_info(ctx, "Source : %s", src_path);
    log_info(ctx, "Output : %s", out_path);
    log_info(ctx, "CPU    : %s (%d-bit, %s-endian)",
             cpu_arch_cli_name(ctx->cpu_arch), cpu_arch_bits(ctx->cpu_arch),
             cpu_arch_endian(ctx->cpu_arch));
    log_info(ctx, "Hardening level: %d", ctx->hardening_level);

    if (!validate_path_for_ctx(ctx, src_path, "source", 0) ||
        !validate_path_for_ctx(ctx, out_path, "output", 1) ||
        (ctx->safety_report_path[0] &&
         !validate_path_for_ctx(ctx, ctx->safety_report_path, "safety report", 1)) ||
        (ctx->diagnostics_report_path[0] &&
         !validate_path_for_ctx(ctx, ctx->diagnostics_report_path, "diagnostics report", 1)) ||
        (ctx->security_report_path[0] &&
         !validate_path_for_ctx(ctx, ctx->security_report_path, "security report", 1)) ||
        (ctx->graph_report_path[0] &&
         !validate_path_for_ctx(ctx, ctx->graph_report_path, "logic graph", 1))) {
        if (ctx->json_mode) emit_json_diagnostics(ctx, 0);
        return 0;
    }

    if (ctx->min_sil > 0 || ctx->require_else || ctx->require_estop ||
        ctx->fail_on_safety_critical)
        ctx->run_safety = 1;

    if ((ctx->expected_source_hash[0] || ctx->security_report_path[0]) &&
        !sec_sha256_file(src_path, ctx->source_hash)) {
        log_error(ctx, 0, "Security: failed to hash source file '%s'", src_path);
        if (ctx->json_mode) emit_json_diagnostics(ctx, 0);
        return 0;
    }

    if (ctx->expected_source_hash[0] &&
        !sec_hash_equal(ctx->source_hash, ctx->expected_source_hash)) {
        log_error(ctx, 0, "Security: source hash mismatch for '%s'", src_path);
        if (ctx->json_mode) emit_json_diagnostics(ctx, 0);
        return 0;
    }

    /* 1. Read source */
    char *source = file_read_limited(src_path, ctx->max_source_bytes);
    if (!source) {
        log_error(ctx, 0, "Failed to read source file '%s'", src_path);
        if (ctx->json_mode) emit_json_diagnostics(ctx, 0);
        return 0;
    }

    /* 2. Lex */
    if (!lexer_tokenize(ctx, source)) {
        free(source);
        if (ctx->json_mode) emit_json_diagnostics(ctx, 0);
        return 0;
    }
    if (ctx->dump_tokens) lexer_dump_tokens(ctx);

    /* 3. Parse */
    ctx->ast_root = parser_parse(ctx);
    if (!ctx->ast_root || ctx->error_count > 0) {
        log_error(ctx, 0, "Parse phase failed with %d errors", ctx->error_count);
        free(source);
        if (ctx->json_mode) emit_json_diagnostics(ctx, 0);
        return 0;
    }

    /* 4. Print AST */
    if (ctx->dump_ast && !ctx->json_mode) {
        printf("\n╔══════════════════════════════════════╗\n");
        printf("║         ABSTRACT SYNTAX TREE         ║\n");
        printf("╚══════════════════════════════════════╝\n");
        ast_print(ctx->ast_root, 0);
    }

    /* 5. Semantic analysis */
    if (!semantic_analyze(ctx)) {
        log_error(ctx, 0, "Semantic analysis failed with %d errors", ctx->error_count);
        free(source);
        if (ctx->json_mode) emit_json_diagnostics(ctx, 0);
        return 0;
    }
    if (ctx->dump_symbols) sym_dump(ctx);

    /* 6. Safety analysis (v2.0) */
    if (ctx->run_safety) {
        safety_analyze(ctx, &ctx->safety_result);
        if (!ctx->json_mode && !ctx->quiet_mode)
            safety_print_report(&ctx->safety_result);
        if (ctx->safety_report_path[0])
            safety_write_report(&ctx->safety_result, ctx->safety_report_path);
        if (ctx->fail_on_safety_critical && ctx->safety_result.critical_count > 0) {
            log_error(ctx, 0, "Security: safety-critical findings are not allowed in this profile");
        }
        if (ctx->require_else && ctx->safety_result.has_missing_else) {
            log_error(ctx, 0, "Security: missing ELSE/default safe-state paths are not allowed");
        }
        if (ctx->require_estop && !ctx->safety_result.has_estop) {
            log_error(ctx, 0, "Security: emergency-stop coverage is required");
        }
        if (ctx->min_sil > 0 && (int)ctx->safety_result.sil_rating < ctx->min_sil) {
            log_error(ctx, 0, "Security: SIL %d is below required SIL %d",
                      (int)ctx->safety_result.sil_rating, ctx->min_sil);
        }
        if (ctx->error_count > 0) {
            free(source);
            if (ctx->json_mode) emit_json_diagnostics(ctx, 0);
            return 0;
        }
    }

    /* 7. Optimizer (v2.0) */
    if (ctx->run_optimizer) {
        optimizer_run(ctx);
    }

    /* 8. Diagnostics */
    if (ctx->run_diagnostics) {
        DiagReport diag_report;
        if (diag_run(ctx, &diag_report)) {
            ctx->diagnostics_quality_score = diag_report.quality_score;
            if (!ctx->json_mode && !ctx->quiet_mode)
                diag_print_report(&diag_report, (DiagOutputFormat)ctx->diagnostics_format);
            if (ctx->diagnostics_report_path[0])
                diag_write_report(&diag_report, ctx->diagnostics_report_path,
                                  (DiagOutputFormat)ctx->diagnostics_format);
            if (ctx->fail_on_warnings && diag_report.warn_count > 0) {
                log_error(ctx, 0, "Security: diagnostics warnings are not allowed in this profile");
            }
        }
    }

    if (ctx->fail_on_warnings && ctx->warning_count > 0)
        log_error(ctx, 0, "Security: compiler warnings are not allowed in this profile");

    if (ctx->error_count > 0) {
        free(source);
        if (ctx->json_mode) emit_json_diagnostics(ctx, 0);
        return 0;
    }

    if (ctx->graph_report_path[0] &&
        !graph_write_dot(ctx, ctx->graph_report_path)) {
        log_error(ctx, 0, "Graph: failed to write logic graph '%s'",
                  ctx->graph_report_path);
        free(source);
        if (ctx->json_mode) emit_json_diagnostics(ctx, 0);
        return 0;
    }

    /* 9. Simulate */
    if (ctx->run_simulation && !ctx->json_mode) simulate_run(ctx);

    /* 10. Code generation */
    int ok;
    if (ctx->fmt == FMT_PLCOPEN_XML) {
        ok = export_plcopen(ctx, out_path);
    } else {
        ok = codegen_run(ctx, out_path);
    }

    if (ok && ctx->security_report_path[0]) {
        if (!sec_sha256_file(out_path, ctx->output_hash)) {
            log_error(ctx, 0, "Security: failed to hash output file '%s'", out_path);
            ok = 0;
        } else if (!sec_write_integrity_report(ctx->security_report_path,
                                               src_path,
                                               out_path,
                                               ctx->source_hash,
                                               ctx->output_hash,
                                               (int)ctx->target,
                                               (int)ctx->fmt,
                                               cpu_arch_cli_name(ctx->cpu_arch),
                                               ctx->hardening_level,
                                               ctx->deterministic_output,
                                               ok,
                                               ctx->sym_count,
                                               ctx->nodes_allocated,
                                               ctx->run_safety ? (int)ctx->safety_result.sil_rating : -1,
                                               ctx->diagnostics_quality_score)) {
            log_error(ctx, 0, "Security: failed to write security report '%s'",
                      ctx->security_report_path);
            ok = 0;
        }
    }

    /* 11. Timing */
    ctx->compile_time_ms = get_time_ms() - t_start;

    /* 12. Summary / diagnostics */
    int success = ok && (ctx->error_count == 0);
    if (!ctx->json_mode) {
        if (!ctx->quiet_mode) {
            printf("\n╔════════════════════════════════════════════════╗\n");
            printf("║              COMPILATION SUMMARY                 ║\n");
            printf("╠════════════════════════════════════════════════╣\n");
            printf("║  Tokens      : %-32d ║\n", ctx->token_count);
            printf("║  AST Nodes   : %-32d ║\n", ctx->nodes_allocated);
            printf("║  Symbols     : %-32d ║\n", ctx->sym_count);
            printf("║  Timers      : %-32d ║\n", ctx->timer_count);
            printf("║  Warnings    : %-32d ║\n", ctx->warning_count);
            printf("║  Errors      : %-32d ║\n", ctx->error_count);
            printf("║  Time (ms)   : %-32.2f ║\n", ctx->compile_time_ms);
            if (ctx->run_safety)
                printf("║  SIL Rating  : SIL %-28d ║\n", ctx->safety_result.sil_rating);
            if (ctx->run_diagnostics)
                printf("║  Quality     : %-29d/100 ║\n", ctx->diagnostics_quality_score);
            printf("║  Status      : %-32s ║\n", success ? "SUCCESS ✓" : "FAILED ✗");
            printf("╚════════════════════════════════════════════════╝\n\n");
        }

        if (ctx->error_count > 0 && !ctx->quiet_mode) {
            printf("Error list:\n");
            for (int i = 0; i < ctx->error_count; i++)
                printf("  [%d] %s\n", i + 1, ctx->errors[i]);
            printf("\n");
        }
    } else {
        char esc_msg[MAX_ERROR_MSG * 2];
        printf("{");
        printf("\"ok\":%s", success ? "true" : "false");
        printf(",\"target\":%d", (int)ctx->target);
        printf(",\"format\":%d", (int)ctx->fmt);
        printf(",\"stats\":{");
        printf("\"tokens\":%d", ctx->token_count);
        printf(",\"ast_nodes\":%d", ctx->nodes_allocated);
        printf(",\"symbols\":%d", ctx->sym_count);
        printf(",\"timers\":%d", ctx->timer_count);
        printf(",\"warnings\":%d", ctx->warning_count);
        printf(",\"errors\":%d", ctx->error_count);
        if (ctx->run_diagnostics)
            printf(",\"quality_score\":%d", ctx->diagnostics_quality_score);
        printf("}");
        printf(",\"errors\":[");
        for (int i = 0; i < ctx->error_count; i++) {
            if (i) printf(",");
            json_escape(ctx->error_messages[i], esc_msg, sizeof(esc_msg));
            printf("{\"line\":%d,\"message\":\"%s\"}", ctx->error_lines[i], esc_msg);
        }
        printf("]");
        printf("}\n");
    }

    free(source);
    return success ? 1 : 0;
}
