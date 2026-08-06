/**
 * plc_compiler.h — PLC DSL Compiler v2.0 Master Header
 * IEC 61131-3 / IEC 61508 — Siemens / CODESYS / Rockwell
 */

#ifndef PLC_COMPILER_H
#define PLC_COMPILER_H

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_MSC_VER)
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

#include "gc.h"

/* ─── Platform ──────────────────────────────────────────────────────────── */
#if defined(__x86_64__) || defined(_M_X64)
#define HOST_CPU_ARCH "x86_64"
#elif defined(__i386__) || defined(_M_IX86)
#define HOST_CPU_ARCH "x86"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define HOST_CPU_ARCH "aarch64"
#elif defined(__arm__) || defined(_M_ARM)
#define HOST_CPU_ARCH "arm"
#elif defined(__riscv) && (__riscv_xlen == 64)
#define HOST_CPU_ARCH "riscv64"
#elif defined(__riscv)
#define HOST_CPU_ARCH "riscv32"
#elif defined(__powerpc64__) || defined(__ppc64__)
#define HOST_CPU_ARCH "ppc64"
#elif defined(__mips64)
#define HOST_CPU_ARCH "mips64"
#elif defined(__mips__)
#define HOST_CPU_ARCH "mips32"
#elif defined(__sparc__) && defined(__arch64__)
#define HOST_CPU_ARCH "sparc64"
#else
#define HOST_CPU_ARCH "unknown-cpu"
#endif

#ifdef _WIN32
#define PLATFORM_OS "Windows"
#elif defined(__linux__)
#define PLATFORM_OS "Linux"
#elif defined(__APPLE__)
#define PLATFORM_OS "macOS"
#else
#define PLATFORM_OS "UnknownOS"
#endif

#define PLATFORM PLATFORM_OS " (" HOST_CPU_ARCH ")"

/* ─── Limits ────────────────────────────────────────────────────────────── */
/* No duplicate includes here anymore */

#define MAX_IDENTIFIER_LEN 128
#define MAX_ERROR_MSG 512
#define MAX_ERRORS 256
#define MAX_LOG_PATH 512
#define MAX_CHILDREN 32
#define SYM_HASH_SIZE 1024
#define DEFAULT_MAX_SOURCE_BYTES (16u * 1024u * 1024u)
#define DEFAULT_MAX_TOKENS 1000000
#define DEFAULT_MAX_AST_NODES 250000
#define DEFAULT_MAX_SYMBOLS 65536
#define MAX_PARSE_CONDITION_DEPTH 256

/* ─── Token types ───────────────────────────────────────────────────────── */
typedef enum {
  TOK_UNKNOWN = 0,
  TOK_IF,
  TOK_THEN,
  TOK_ELSE,
  TOK_END,
  TOK_FOR,
  TOK_AND,
  TOK_OR,
  TOK_NOT,
  TOK_ON,
  TOK_OFF,
  TOK_WHILE,
  TOK_DO,
  TOK_CASE,
  TOK_OF,
  TOK_DEFAULT,
  TOK_END_CASE,
  TOK_REPEAT,
  TOK_UNTIL,
  TOK_STRUCT,
  TOK_END_STRUCT,
  TOK_FUNCTION_BLOCK,
  TOK_END_FUNCTION_BLOCK,
  TOK_VAR,
  TOK_VAR_INPUT,
  TOK_VAR_OUTPUT,
  TOK_END_VAR,
  TOK_STATE_MACHINE,
  TOK_END_STATE_MACHINE,
  TOK_STATE,
  TOK_TRANSITION,
  TOK_TO,
  TOK_ASSERT,
  TOK_PRE,
  TOK_POST,
  TOK_INVARIANT,
  TOK_ENTRY,
  TOK_EXIT,
  TOK_CONST,
  TOK_USE,
  TOK_IN,
  TOK_DOC_COMMENT,
  TOK_EQ,
  TOK_NEQ,
  TOK_GTE,
  TOK_LTE,
  TOK_GT,
  TOK_LT,
  TOK_NUMBER,
  TOK_IDENTIFIER,
  TOK_STRING_LITERAL,
  TOK_SECONDS,
  TOK_MINUTES,
  TOK_MILLISECONDS,
  TOK_SEMICOLON,
  TOK_COLON,
  TOK_LBRACKET,
  TOK_RBRACKET,
  TOK_LPAREN,
  TOK_RPAREN,
  TOK_AT,           /* @ — annotation prefix                          */
  TOK_ANNOTATION,   /* @CRITICAL @ESTOP @SIL1 @SIL2 @SIL3 @SIL4      */
  TOK_EOF
} PlcTokenType;

typedef struct {
  PlcTokenType type;
  char value[MAX_IDENTIFIER_LEN];
  int line;
  int col;
} Token;

/* ─── AST ───────────────────────────────────────────────────────────────── */
typedef enum {
  NODE_PROGRAM = 0,
  NODE_IF,
  NODE_WHILE,
  NODE_CASE,
  NODE_CASE_BRANCH,
  NODE_REPEAT,
  NODE_COMPARISON,
  NODE_AND,
  NODE_OR,
  NODE_NOT,
  NODE_ACTION,
  NODE_STRUCT,
  NODE_FUNCTION_BLOCK,
  NODE_STATE_MACHINE,
  NODE_STATE,
  NODE_TRANSITION,
  NODE_ASSERT,
  NODE_CONTRACT,
  NODE_STMT_LIST,
  NODE_VAR_DECL,
  NODE_DOC_COMMENT,
  NODE_CONST_DECL,
  NODE_RECIPE_USE
} NodeType;

typedef enum { CMP_EQ = 0, CMP_NEQ, CMP_GTE, CMP_LTE, CMP_GT, CMP_LT } CmpOp;
typedef enum { TIMER_SECONDS = 0, TIMER_MINUTES, TIMER_MILLISECONDS } TimerUnit;

typedef struct ASTNode {
  NodeType type;
  int line;
  char var_name[MAX_IDENTIFIER_LEN];
  char value[MAX_IDENTIFIER_LEN];
  char unit[MAX_IDENTIFIER_LEN];
  double numeric_val;
  int is_numeric;
  CmpOp cmp_op;
  int has_timer;
  double timer_value;
  TimerUnit timer_unit;
  int timer_symbol_index;
  int safety_critical;
  int safety_estop;
  int safety_sil_level;
  struct ASTNode *children[MAX_CHILDREN];
  int child_count;
  struct ASTNode *next;
} ASTNode;

/* ─── Symbol table ──────────────────────────────────────────────────────── */
typedef enum {
  IO_INPUT = 0,
  IO_OUTPUT,
  IO_MEMORY,
  IO_TIMER_VAR,
  IO_VAR_LOCAL
} IODirection;
typedef enum {
  SYM_BOOL = 0,
  SYM_INT,
  SYM_REAL,
  SYM_TIMER,
  SYM_STRUCT,
  SYM_FUNCTION_BLOCK,
  SYM_INSTANCE,
  SYM_CONST
} SymbolKind;

typedef struct Symbol {
  char name[MAX_IDENTIFIER_LEN];
  char st_name[MAX_IDENTIFIER_LEN];
  char plc_address[MAX_IDENTIFIER_LEN];
  IODirection direction;
  SymbolKind kind;
  int is_used;
  int timer_index;
  int scope_id;
  int line;              /* Declaration line number                    */
  int col;               /* Declaration column number                  */
  int safety_critical;  /* 1 = @CRITICAL annotation present            */
  int safety_estop;     /* 1 = @ESTOP annotation present               */
  int safety_sil_level; /* 0 = none, 1-4 = @SIL1-@SIL4 annotation     */
  char unit[MAX_IDENTIFIER_LEN];
  char const_value[MAX_IDENTIFIER_LEN];
  char parent_type[MAX_IDENTIFIER_LEN];
  struct Symbol *next_in_hash;
} Symbol;

/* ─── Target / format ───────────────────────────────────────────────────── */
typedef enum { PLC_SIEMENS_TIA = 0, PLC_CODESYS, PLC_ROCKWELL_AOI } PLCTarget;
typedef enum {
  FMT_STRUCTURED_TEXT = 0,
  FMT_LADDER_LOGIC,
  FMT_PLCOPEN_XML
} OutputFormat;

typedef enum {
  CPU_ARCH_AUTO = 0,
  CPU_ARCH_X86,
  CPU_ARCH_X86_64,
  CPU_ARCH_ARMV7,
  CPU_ARCH_AARCH64,
  CPU_ARCH_RISCV32,
  CPU_ARCH_RISCV64,
  CPU_ARCH_PPC64,
  CPU_ARCH_MIPS32,
  CPU_ARCH_MIPS64,
  CPU_ARCH_SPARC64,
  CPU_ARCH_WASM32
} CPUArch;

typedef enum {
  HARDENING_STANDARD = 0,
  HARDENING_STRICT = 1,
  HARDENING_PARANOID = 2
} HardeningLevel;

/* ─── Safety result (defined inline to avoid circular include) ──────────── */
#define MAX_SAFETY_MSG 256
#define MAX_SAFETY_ISSUES 256
#define MAX_OUTPUT_RULES 64
#define MAX_OUTPUT_TRACKERS 256

typedef enum {
  SAFETY_INFO = 0,
  SAFETY_WARNING,
  SAFETY_CRITICAL,
  SAFETY_FATAL
} SafetySeverity;
typedef enum {
  SAFE_CAT_CONFLICTING_OUTPUTS = 0,
  SAFE_CAT_MISSING_ELSE,
  SAFE_CAT_ESTOP_COVERAGE,
  SAFE_CAT_TIMER_ON_CRITICAL_PATH,
  SAFE_CAT_WRITE_WRITE_CONFLICT,
  SAFE_CAT_UNREACHABLE_CODE,
  SAFE_CAT_UNINITIALIZED_OUTPUT,
  SAFE_CAT_REDUNDANCY,
  SAFE_CAT_CONDITION_OVERLAP,
  SAFE_CAT_CIRCULAR_DEPENDENCY,
  SAFE_CAT_COMPLEXITY,
  SAFE_CAT_GENERAL
} SafetyCategory;
typedef enum {
  SIL_NONE = 0,
  SIL_1 = 1,
  SIL_2 = 2,
  SIL_3 = 3,
  SIL_4 = 4
} SILLevel;

typedef struct {
  SafetySeverity severity;
  SafetyCategory category;
  int line;
  char message[MAX_SAFETY_MSG];
  char recommendation[MAX_SAFETY_MSG];
  char variable[MAX_IDENTIFIER_LEN];
} SafetyIssue;

typedef struct {
  char name[MAX_IDENTIFIER_LEN];
  int rule_indices[MAX_OUTPUT_RULES];
  int values[MAX_OUTPUT_RULES];
  int rule_count;
  int has_on, has_off, has_else_path;
  int covered_by_estop, depends_on_timer;
} OutputTracker;

typedef struct {
  SafetyIssue issues[MAX_SAFETY_ISSUES];
  int issue_count;
  SILLevel sil_rating;
  int compliance_score;
  int critical_count, warning_count, info_count;
  int total_outputs, covered_outputs;
  int estop_covered, has_estop;
  int has_conflicting_outputs, has_missing_else;
  int has_timer_on_critical, has_write_write_conflict;
  OutputTracker outputs[MAX_OUTPUT_TRACKERS];
  int output_count;
} SafetyResult;

/* ─── Compiler context ──────────────────────────────────────────────────── */
typedef struct CompilerCtx {
  PLCTarget target;
  OutputFormat fmt;

  int json_mode, quiet_mode;
  int dump_tokens, dump_ast, dump_symbols;
  int run_simulation, run_safety, run_optimizer;
  int run_diagnostics, diagnostics_format, diagnostics_quality_score;
  int print_generated_output;
  int deterministic_output;
  int allow_output_overwrite;
  int sandbox_paths;
  int reject_output_symlink;
  int fail_on_warnings;
  int fail_on_safety_critical;
  int require_else;
  int require_estop;
  int min_sil;
  int hardening_level;
  CPUArch cpu_arch;
  unsigned int max_source_bytes;
  int max_tokens;
  int max_ast_nodes;
  int max_symbols;

  char log_path[MAX_LOG_PATH];
  char safety_report_path[MAX_LOG_PATH];
  char diagnostics_report_path[MAX_LOG_PATH];
  char security_report_path[MAX_LOG_PATH];
  char graph_report_path[MAX_LOG_PATH];
  char expected_source_hash[65];
  char source_hash[65];
  char output_hash[65];
  FILE *log_file;

  char *source;
  int src_len, src_pos, cur_line, cur_col;

  Token *tokens;
  int token_count, token_cap, token_pos;

  ASTNode *ast_root;
  int nodes_allocated, nodes_freed;

  Symbol *symbols;
  int sym_count, sym_cap;
  Symbol *sym_hash[SYM_HASH_SIZE];

  int timer_count, warning_count, error_count;
  int cur_scope;
  int next_scope_id;

  char errors[MAX_ERRORS][MAX_ERROR_MSG];
  int error_lines[MAX_ERRORS];
  char error_messages[MAX_ERRORS][MAX_ERROR_MSG];

  double compile_time_ms;

  SafetyResult safety_result;

  int use_arena_gc;
  GcArena arena;
} CompilerCtx;

/* ─── Public API ────────────────────────────────────────────────────────── */
CompilerCtx *compiler_create(PLCTarget target, OutputFormat fmt);
void compiler_destroy(CompilerCtx *ctx);
int compiler_compile(CompilerCtx *ctx, const char *src_path,
                     const char *out_path);
char *file_read(const char *path);
int file_write(const char *path, const char *content);
int compiler_write_output(CompilerCtx *ctx, const char *path,
                          const char *content);
void simulate_run(CompilerCtx *ctx);

void log_init(CompilerCtx *ctx, const char *path);
void log_info(CompilerCtx *ctx, const char *fmt, ...);
void log_warn(CompilerCtx *ctx, const char *fmt, ...);
void log_error(CompilerCtx *ctx, int line, const char *fmt, ...);
void log_close(CompilerCtx *ctx);
void print_banner(void);

int lexer_tokenize(CompilerCtx *ctx, const char *source);
void lexer_dump_tokens(CompilerCtx *ctx);
const char *token_type_name(PlcTokenType t);

ASTNode *parser_parse(CompilerCtx *ctx);
void ast_print(ASTNode *node, int depth);
void ast_free(CompilerCtx *ctx, ASTNode *node);

Symbol *sym_lookup(CompilerCtx *ctx, const char *name);
Symbol *sym_lookup_scoped(CompilerCtx *ctx, const char *name, int scope_id);
Symbol *sym_insert(CompilerCtx *ctx, const char *name, IODirection dir,
                   SymbolKind kind, int line, int col);
Symbol *sym_insert_scoped(CompilerCtx *ctx, const char *name, IODirection dir,
                          SymbolKind kind, int scope_id,
                          const char *parent_type, int line, int col);
void sym_assign_addresses(CompilerCtx *ctx);
void sym_dump(CompilerCtx *ctx);

int semantic_analyze(CompilerCtx *ctx);
int codegen_run(CompilerCtx *ctx, const char *out_path);
int optimizer_run(CompilerCtx *ctx);
int export_plcopen(CompilerCtx *ctx, const char *out_path);
int translate_vendor_code(const char *src_path, const char *out_path,
                          PLCTarget from, PLCTarget to, int quiet_mode);

int safety_analyze(CompilerCtx *ctx, SafetyResult *result);
void safety_print_report(const SafetyResult *result);
void safety_emit_json(const SafetyResult *result);
int safety_write_report(const SafetyResult *result, const char *path);

int graph_write_dot(CompilerCtx *ctx, const char *path);

const char *cpu_arch_name(CPUArch arch);
const char *cpu_arch_cli_name(CPUArch arch);
const char *cpu_arch_endian(CPUArch arch);
int cpu_arch_bits(CPUArch arch);
int cpu_arch_parse(const char *name, CPUArch *arch);
void cpu_arch_print_supported(FILE *out);
void compiler_apply_hardening_profile(CompilerCtx *ctx, HardeningLevel level);

/* ─── Shared ST Generation (v2.0) ───────────────────────────────────────── */
void st_gen_condition(CompilerCtx *ctx, char *out, int max, ASTNode *node);
void st_gen_actions(CompilerCtx *ctx, char *out, int max, ASTNode *head,
                    int indent);
void st_gen_statement(CompilerCtx *ctx, char *out, int max, ASTNode *node,
                      int indent);

/* ─── Security Helpers ──────────────────────────────────────────────────── */
double safe_atof(const char *str);

#endif /* PLC_COMPILER_H */
