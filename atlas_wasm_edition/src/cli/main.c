/**
 * main.c — PLC DSL Compiler v2.0 Entry Point
 * Usage: plc_compiler <input.dsl> <output> [options]
 */

#include "plc_compiler.h"
#include "security.h"
#include <errno.h>

static int parse_target_name(const char *name, PLCTarget *target) {
    if (strcmp(name, "siemens") == 0) *target = PLC_SIEMENS_TIA;
    else if (strcmp(name, "codesys") == 0) *target = PLC_CODESYS;
    else if (strcmp(name, "rockwell") == 0) *target = PLC_ROCKWELL_AOI;
    else return 0;
    return 1;
}

static int parse_diagnostics_format(const char *name, int *fmt) {
    if (strcmp(name, "text") == 0) *fmt = 0;
    else if (strcmp(name, "json") == 0) *fmt = 1;
    else if (strcmp(name, "markdown") == 0 || strcmp(name, "md") == 0) *fmt = 2;
    else if (strcmp(name, "html") == 0) *fmt = 3;
    else return 0;
    return 1;
}

static int parse_hardening_name(const char *name, HardeningLevel *level) {
    if (strcmp(name, "standard") == 0 || strcmp(name, "0") == 0) *level = HARDENING_STANDARD;
    else if (strcmp(name, "strict") == 0 || strcmp(name, "1") == 0) *level = HARDENING_STRICT;
    else if (strcmp(name, "paranoid") == 0 || strcmp(name, "2") == 0) *level = HARDENING_PARANOID;
    else return 0;
    return 1;
}

static int parse_positive_uint(const char *text, unsigned int *out) {
    char *end = NULL;
    unsigned long value;
    if (!text || !out) return 0;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || end == text || (end && *end) || value == 0 || value > 2147483647ul)
        return 0;
    *out = (unsigned int)value;
    return 1;
}

static int verify_builds(const char *a, const char *b, int json_mode) {
    char hash_a[SEC_SHA256_HEX_LEN];
    char hash_b[SEC_SHA256_HEX_LEN];

    if (!sec_sha256_file(a, hash_a)) {
        if (json_mode)
            printf("{\"ok\":false,\"error\":\"failed_to_hash_first_file\"}\n");
        else
            fprintf(stderr, "Verify failed: could not hash '%s'\n", a);
        return 1;
    }
    if (!sec_sha256_file(b, hash_b)) {
        if (json_mode)
            printf("{\"ok\":false,\"error\":\"failed_to_hash_second_file\"}\n");
        else
            fprintf(stderr, "Verify failed: could not hash '%s'\n", b);
        return 1;
    }

    int match = sec_hash_equal(hash_a, hash_b);
    if (json_mode) {
        printf("{\"ok\":%s,\"file_a_sha256\":\"%s\",\"file_b_sha256\":\"%s\"}\n",
               match ? "true" : "false", hash_a, hash_b);
    } else {
        printf("Build A SHA-256: %s\n", hash_a);
        printf("Build B SHA-256: %s\n", hash_b);
        printf("Deterministic verify: %s\n", match ? "PASS" : "FAIL");
    }
    return match ? 0 : 2;
}

static void usage(const char *prog) {
    printf("PLC DSL Compiler v2.0 — IEC 61131-3 / IEC 61508\n\n");
    printf("Usage: %s <input.dsl> <output> [options]\n\n", prog);
    printf("Output formats:\n");
    printf("  --format st        Structured Text (default)\n");
    printf("  --format ladder    Ladder Logic\n");
    printf("  --format plcopen   PLCopen XML (IEC 61131-10)\n");
    printf("\nTarget platforms:\n");
    printf("  --target siemens   Siemens TIA Portal (default)\n");
    printf("  --target codesys   CODESYS V3\n");
    printf("  --target rockwell  Rockwell Studio 5000\n");
    printf("\nCPU profiles / deployment metadata:\n");
    printf("  --cpu <arch>       CPU profile: x86_64, x86, armv7, aarch64, riscv32, riscv64,\n");
    printf("                     ppc64, mips32, mips64, sparc64, wasm32\n");
    printf("  --list-cpus        Print supported CPU profiles\n");
    printf("\nSafety & Optimization (v2.0):\n");
    printf("  --safety                 IEC 61508 safety analysis with SIL rating\n");
    printf("  --safety-report <path>   Write safety report to file\n");
    printf("  --optimize               Run AST optimization pass\n");
    printf("  --diagnostics            Run code quality diagnostics\n");
    printf("  --diagnostics-report <path>  Write diagnostics report to file\n");
    printf("  --diagnostics-format <text|json|markdown|html>\n");
    printf("  --security-report <path>     Write source/output SHA-256 integrity report\n");
    printf("  --expect-source-hash <sha256>  Fail if source hash does not match\n");
    printf("  --graph <path>       Write Graphviz DOT logic/safety graph\n");
    printf("  --hardening <standard|strict|paranoid>  Enable resource/security gates\n");
    printf("  --industrial        Paranoid hardening, deterministic output, safety/diagnostics gates\n");
    printf("  --strict            Fail on warnings and safety-critical findings\n");
    printf("  --min-sil <1-4>     Fail unless safety analysis reaches this SIL level\n");
    printf("  --require-estop     Fail unless an emergency-stop rule is present\n");
    printf("  --require-else      Fail if output rules omit ELSE/default safe-state paths\n");
    printf("  --fail-on-safety    Fail if safety analysis reports critical findings\n");
    printf("  --fail-on-warnings  Fail if compiler or diagnostics warnings are emitted\n");
    printf("  --deterministic     Use reproducible timestamps in generated artifacts\n");
    printf("  --no-overwrite      Refuse to replace an existing output file\n");
    printf("  --allow-overwrite   Permit overwriting output after a stricter profile enables refusal\n");
    printf("  --sandbox-paths     Require relative paths and reject '..' traversal\n");
    printf("  --max-source-bytes <n>  Limit source file size\n");
    printf("  --max-tokens <n>    Limit lexer tokens\n");
    printf("  --max-ast-nodes <n> Limit AST nodes\n");
    printf("  --max-symbols <n>   Limit symbol table entries\n");
    printf("\nDiagnostics / debugging:\n");
    printf("  --json             Emit JSON diagnostics only (machine-readable)\n");
    printf("  --quiet            Suppress INFO/WARN console output\n");
    printf("  --dump-tokens      Print token stream\n");
    printf("  --dump-ast         Print AST\n");
    printf("  --dump-symbols     Print symbol table\n");
    printf("  --sim              Run simulation pass\n");
    printf("  --log <path>       Write log file to <path>\n");
    printf("  --no-print-output  Do not print generated code to console\n");
    printf("\nVendor translation mode:\n");
    printf("  --translate-from <siemens|codesys|rockwell>\n");
    printf("  --translate-to   <siemens|codesys|rockwell>\n");
    printf("\nVerification mode:\n");
    printf("  %s verify <build-a> <build-b> [--json]\n", prog);
    printf("\nExamples:\n");
    printf("  %s program.dsl output.st\n", prog);
    printf("  %s program.dsl output.st --target codesys --safety\n", prog);
    printf("  %s program.dsl output.xml --format plcopen --safety\n", prog);
    printf("  %s program.dsl output.ladder --format ladder --target rockwell --optimize\n", prog);
    printf("  %s siemens.st rockwell.st --translate-from siemens --translate-to rockwell\n\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc == 2 && strcmp(argv[1], "--list-cpus") == 0) {
        cpu_arch_print_supported(stdout);
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        usage(argv[0]);
        return 0;
    }
    if (argc >= 4 && strcmp(argv[1], "verify") == 0) {
        int json_mode = (argc >= 5 && strcmp(argv[4], "--json") == 0);
        if (argc > 5 || (argc == 5 && !json_mode)) {
            fprintf(stderr, "Usage: %s verify <build-a> <build-b> [--json]\n", argv[0]);
            return 1;
        }
        return verify_builds(argv[2], argv[3], json_mode);
    }
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *src_path = argv[1];
    const char *out_path = argv[2];

    PLCTarget    target = PLC_SIEMENS_TIA;
    OutputFormat fmt    = FMT_STRUCTURED_TEXT;

    /* Parse optional flags */
    int json_mode = 0;
    int quiet_mode = 0;
    int dump_tokens = 0;
    int dump_ast = 0;
    int dump_symbols = 0;
    int run_simulation = 0;
    int run_safety = 0;
    int run_optimizer = 0;
    int run_diagnostics = 0;
    int diagnostics_format = 0;
    int print_generated_output = 1;
    int cpu_arch_set = 0;
    CPUArch cpu_arch = CPU_ARCH_AUTO;
    int hardening_set = 0;
    HardeningLevel hardening_level = HARDENING_STANDARD;
    int deterministic_override = -1;
    int overwrite_override = -1;
    int sandbox_override = -1;
    int fail_warnings_override = -1;
    int fail_safety_override = -1;
    int require_else_override = -1;
    int require_estop_override = -1;
    int min_sil_override = -1;
    unsigned int max_source_bytes_override = 0;
    unsigned int max_tokens_override = 0;
    unsigned int max_ast_nodes_override = 0;
    unsigned int max_symbols_override = 0;
    const char *log_path = NULL;
    const char *safety_report_path = NULL;
    const char *diagnostics_report_path = NULL;
    const char *security_report_path = NULL;
    const char *graph_report_path = NULL;
    const char *expected_source_hash = NULL;
    int translate_mode = 0;
    PLCTarget translate_from = PLC_SIEMENS_TIA;
    PLCTarget translate_to = PLC_SIEMENS_TIA;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            i++;
            if (!parse_target_name(argv[i], &target))
                { fprintf(stderr, "Unknown target: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
            i++;
            if (!cpu_arch_parse(argv[i], &cpu_arch))
                { fprintf(stderr, "Unknown CPU profile: %s\n", argv[i]); return 1; }
            cpu_arch_set = 1;
        } else if (strcmp(argv[i], "--cpu") == 0) {
            fprintf(stderr, "Missing value for --cpu\n");
            return 1;
        } else if (strcmp(argv[i], "--list-cpus") == 0) {
            cpu_arch_print_supported(stdout);
            return 0;
        } else if (strcmp(argv[i], "--translate-from") == 0 && i + 1 < argc) {
            i++;
            if (!parse_target_name(argv[i], &translate_from))
                { fprintf(stderr, "Unknown translation source: %s\n", argv[i]); return 1; }
            translate_mode = 1;
        } else if (strcmp(argv[i], "--translate-to") == 0 && i + 1 < argc) {
            i++;
            if (!parse_target_name(argv[i], &translate_to))
                { fprintf(stderr, "Unknown translation target: %s\n", argv[i]); return 1; }
            translate_mode = 1;
        } else if (strcmp(argv[i], "--target") == 0) {
            fprintf(stderr, "Missing value for --target\n");
            return 1;
        } else if (strcmp(argv[i], "--translate-from") == 0 || strcmp(argv[i], "--translate-to") == 0) {
            fprintf(stderr, "Missing value for %s\n", argv[i]);
            return 1;
        } else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            i++;
            if      (strcmp(argv[i], "st")     == 0) fmt = FMT_STRUCTURED_TEXT;
            else if (strcmp(argv[i], "ladder") == 0) fmt = FMT_LADDER_LOGIC;
            else if (strcmp(argv[i], "plcopen") == 0) fmt = FMT_PLCOPEN_XML;
            else { fprintf(stderr, "Unknown format: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--format") == 0) {
            fprintf(stderr, "Missing value for --format\n");
            return 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            json_mode = 1;
            quiet_mode = 1;
            print_generated_output = 0;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet_mode = 1;
        } else if (strcmp(argv[i], "--dump-tokens") == 0) {
            dump_tokens = 1;
        } else if (strcmp(argv[i], "--dump-ast") == 0) {
            dump_ast = 1;
        } else if (strcmp(argv[i], "--dump-symbols") == 0) {
            dump_symbols = 1;
        } else if (strcmp(argv[i], "--sim") == 0) {
            run_simulation = 1;
        } else if (strcmp(argv[i], "--safety") == 0) {
            run_safety = 1;
        } else if (strcmp(argv[i], "--safety-report") == 0 && i + 1 < argc) {
            i++;
            safety_report_path = argv[i];
            run_safety = 1;
        } else if (strcmp(argv[i], "--optimize") == 0) {
            run_optimizer = 1;
        } else if (strcmp(argv[i], "--diagnostics") == 0) {
            run_diagnostics = 1;
        } else if (strcmp(argv[i], "--diagnostics-report") == 0 && i + 1 < argc) {
            i++;
            diagnostics_report_path = argv[i];
            run_diagnostics = 1;
        } else if (strcmp(argv[i], "--diagnostics-report") == 0) {
            fprintf(stderr, "Missing value for --diagnostics-report\n");
            return 1;
        } else if (strcmp(argv[i], "--diagnostics-format") == 0 && i + 1 < argc) {
            i++;
            if (!parse_diagnostics_format(argv[i], &diagnostics_format))
                { fprintf(stderr, "Unknown diagnostics format: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--diagnostics-format") == 0) {
            fprintf(stderr, "Missing value for --diagnostics-format\n");
            return 1;
        } else if (strcmp(argv[i], "--security-report") == 0 && i + 1 < argc) {
            i++;
            security_report_path = argv[i];
        } else if (strcmp(argv[i], "--security-report") == 0) {
            fprintf(stderr, "Missing value for --security-report\n");
            return 1;
        } else if (strcmp(argv[i], "--graph") == 0 && i + 1 < argc) {
            i++;
            graph_report_path = argv[i];
            run_safety = 1;
        } else if (strcmp(argv[i], "--graph") == 0) {
            fprintf(stderr, "Missing value for --graph\n");
            return 1;
        } else if (strcmp(argv[i], "--expect-source-hash") == 0 && i + 1 < argc) {
            i++;
            if (!sec_is_sha256_hex(argv[i]))
                { fprintf(stderr, "Invalid SHA-256 hash: %s\n", argv[i]); return 1; }
            expected_source_hash = argv[i];
        } else if (strcmp(argv[i], "--expect-source-hash") == 0) {
            fprintf(stderr, "Missing value for --expect-source-hash\n");
            return 1;
        } else if (strcmp(argv[i], "--hardening") == 0 && i + 1 < argc) {
            i++;
            if (!parse_hardening_name(argv[i], &hardening_level))
                { fprintf(stderr, "Unknown hardening profile: %s\n", argv[i]); return 1; }
            hardening_set = 1;
        } else if (strcmp(argv[i], "--hardening") == 0) {
            fprintf(stderr, "Missing value for --hardening\n");
            return 1;
        } else if (strcmp(argv[i], "--industrial") == 0) {
            hardening_level = HARDENING_PARANOID;
            hardening_set = 1;
        } else if (strcmp(argv[i], "--strict") == 0) {
            fail_warnings_override = 1;
            fail_safety_override = 1;
            require_else_override = 1;
            run_safety = 1;
            run_diagnostics = 1;
        } else if (strcmp(argv[i], "--min-sil") == 0 && i + 1 < argc) {
            unsigned int v;
            i++;
            if (!parse_positive_uint(argv[i], &v) || v > 4)
                { fprintf(stderr, "Invalid SIL level: %s\n", argv[i]); return 1; }
            min_sil_override = (int)v;
            run_safety = 1;
        } else if (strcmp(argv[i], "--min-sil") == 0) {
            fprintf(stderr, "Missing value for --min-sil\n");
            return 1;
        } else if (strcmp(argv[i], "--require-estop") == 0) {
            require_estop_override = 1;
            run_safety = 1;
        } else if (strcmp(argv[i], "--require-else") == 0) {
            require_else_override = 1;
            run_safety = 1;
        } else if (strcmp(argv[i], "--fail-on-safety") == 0) {
            fail_safety_override = 1;
            run_safety = 1;
        } else if (strcmp(argv[i], "--fail-on-warnings") == 0) {
            fail_warnings_override = 1;
        } else if (strcmp(argv[i], "--deterministic") == 0) {
            deterministic_override = 1;
        } else if (strcmp(argv[i], "--no-overwrite") == 0) {
            overwrite_override = 0;
        } else if (strcmp(argv[i], "--allow-overwrite") == 0) {
            overwrite_override = 1;
        } else if (strcmp(argv[i], "--sandbox-paths") == 0) {
            sandbox_override = 1;
        } else if (strcmp(argv[i], "--max-source-bytes") == 0 && i + 1 < argc) {
            i++;
            if (!parse_positive_uint(argv[i], &max_source_bytes_override))
                { fprintf(stderr, "Invalid value for --max-source-bytes: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--max-source-bytes") == 0) {
            fprintf(stderr, "Missing value for --max-source-bytes\n");
            return 1;
        } else if (strcmp(argv[i], "--max-tokens") == 0 && i + 1 < argc) {
            i++;
            if (!parse_positive_uint(argv[i], &max_tokens_override))
                { fprintf(stderr, "Invalid value for --max-tokens: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--max-tokens") == 0) {
            fprintf(stderr, "Missing value for --max-tokens\n");
            return 1;
        } else if (strcmp(argv[i], "--max-ast-nodes") == 0 && i + 1 < argc) {
            i++;
            if (!parse_positive_uint(argv[i], &max_ast_nodes_override))
                { fprintf(stderr, "Invalid value for --max-ast-nodes: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--max-ast-nodes") == 0) {
            fprintf(stderr, "Missing value for --max-ast-nodes\n");
            return 1;
        } else if (strcmp(argv[i], "--max-symbols") == 0 && i + 1 < argc) {
            i++;
            if (!parse_positive_uint(argv[i], &max_symbols_override))
                { fprintf(stderr, "Invalid value for --max-symbols: %s\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "--max-symbols") == 0) {
            fprintf(stderr, "Missing value for --max-symbols\n");
            return 1;
        } else if (strcmp(argv[i], "--no-print-output") == 0) {
            print_generated_output = 0;
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            i++;
            log_path = argv[i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (translate_mode) {
        if (json_mode) {
            fprintf(stderr, "JSON mode is not supported with vendor translation\n");
            return 1;
        }
        return translate_vendor_code(src_path, out_path, translate_from, translate_to, quiet_mode) ? 0 : 1;
    }

    CompilerCtx *ctx = compiler_create(target, fmt);
    if (!ctx) { fprintf(stderr, "Failed to create compiler context\n"); return 1; }

    if (cpu_arch_set)
        ctx->cpu_arch = cpu_arch;

    ctx->json_mode = json_mode;
    ctx->quiet_mode = quiet_mode;
    ctx->dump_tokens = dump_tokens;
    ctx->dump_ast = dump_ast;
    ctx->dump_symbols = dump_symbols;
    ctx->run_simulation = run_simulation;
    ctx->run_safety = run_safety;
    ctx->run_optimizer = run_optimizer;
    ctx->run_diagnostics = run_diagnostics;
    ctx->diagnostics_format = diagnostics_format;
    ctx->print_generated_output = print_generated_output;
    if (hardening_set)
        compiler_apply_hardening_profile(ctx, hardening_level);
    if (deterministic_override >= 0) ctx->deterministic_output = deterministic_override;
    if (overwrite_override >= 0) ctx->allow_output_overwrite = overwrite_override;
    if (sandbox_override >= 0) ctx->sandbox_paths = sandbox_override;
    if (fail_warnings_override >= 0) ctx->fail_on_warnings = fail_warnings_override;
    if (fail_safety_override >= 0) ctx->fail_on_safety_critical = fail_safety_override;
    if (require_else_override >= 0) ctx->require_else = require_else_override;
    if (require_estop_override >= 0) ctx->require_estop = require_estop_override;
    if (min_sil_override >= 0) ctx->min_sil = min_sil_override;
    if (max_source_bytes_override > 0) ctx->max_source_bytes = max_source_bytes_override;
    if (max_tokens_override > 0) ctx->max_tokens = (int)max_tokens_override;
    if (max_ast_nodes_override > 0) ctx->max_ast_nodes = (int)max_ast_nodes_override;
    if (max_symbols_override > 0) ctx->max_symbols = (int)max_symbols_override;
    if (log_path && log_path[0]) {
        strncpy(ctx->log_path, log_path, MAX_LOG_PATH - 1);
        ctx->log_path[MAX_LOG_PATH - 1] = '\0';
    }
    if (safety_report_path && safety_report_path[0]) {
        strncpy(ctx->safety_report_path, safety_report_path, MAX_LOG_PATH - 1);
        ctx->safety_report_path[MAX_LOG_PATH - 1] = '\0';
    }
    if (diagnostics_report_path && diagnostics_report_path[0]) {
        strncpy(ctx->diagnostics_report_path, diagnostics_report_path, MAX_LOG_PATH - 1);
        ctx->diagnostics_report_path[MAX_LOG_PATH - 1] = '\0';
    }
    if (security_report_path && security_report_path[0]) {
        strncpy(ctx->security_report_path, security_report_path, MAX_LOG_PATH - 1);
        ctx->security_report_path[MAX_LOG_PATH - 1] = '\0';
    }
    if (graph_report_path && graph_report_path[0]) {
        strncpy(ctx->graph_report_path, graph_report_path, MAX_LOG_PATH - 1);
        ctx->graph_report_path[MAX_LOG_PATH - 1] = '\0';
    }
    if (expected_source_hash && expected_source_hash[0]) {
        strncpy(ctx->expected_source_hash, expected_source_hash,
                sizeof(ctx->expected_source_hash) - 1);
        ctx->expected_source_hash[sizeof(ctx->expected_source_hash) - 1] = '\0';
    }

    int result = compiler_compile(ctx, src_path, out_path);

    compiler_destroy(ctx);
    return result ? 0 : 1;
}
