/**
 * diagnostics.h - PLC compiler quality diagnostics.
 *
 * This pass complements the safety engine with maintainability checks,
 * complexity metrics, and report output suitable for CI or design reviews.
 */

#ifndef PLC_DIAGNOSTICS_H
#define PLC_DIAGNOSTICS_H

typedef enum {
    DIAG_HINT = 0,
    DIAG_INFO,
    DIAG_WARN,
    DIAG_ERROR
} DiagSeverity;

typedef enum {
    DIAG_DEAD_CODE = 0,
    DIAG_COMPLEXITY,
    DIAG_NAMING,
    DIAG_DEPRECATED,
    DIAG_DUPLICATE,
    DIAG_UNUSED_SYMBOL,
    DIAG_NESTING_DEPTH,
    DIAG_LARGE_PROGRAM,
    DIAG_NO_COMMENT,
    DIAG_MAGIC_NUMBER,
    DIAG_GENERAL
} DiagCategory;

#define MAX_DIAG_ISSUES 512
#define MAX_DIAG_MSG 256

typedef struct {
    DiagSeverity severity;
    DiagCategory category;
    int line;
    char message[MAX_DIAG_MSG];
    char suggestion[MAX_DIAG_MSG];
} DiagIssue;

typedef struct {
    int total_nodes;
    int max_nesting_depth;
    int cyclomatic_complexity;
    int if_count;
    int while_count;
    int case_count;
    int function_block_count;
    int state_machine_count;
    int assert_count;
    int total_symbols;
    int unused_symbols;
    int output_symbols;
    int input_symbols;
    int timer_symbols;
    int source_lines;
} DiagMetrics;

typedef struct {
    DiagIssue issues[MAX_DIAG_ISSUES];
    int issue_count;
    DiagMetrics metrics;
    int hint_count;
    int info_count;
    int warn_count;
    int error_count;
    int quality_score;
} DiagReport;

typedef enum {
    DIAG_FMT_TEXT = 0,
    DIAG_FMT_JSON,
    DIAG_FMT_MARKDOWN,
    DIAG_FMT_HTML
} DiagOutputFormat;

struct CompilerCtx;

int diag_run(struct CompilerCtx *ctx, DiagReport *report);
void diag_print_report(const DiagReport *report, DiagOutputFormat fmt);
int diag_write_report(const DiagReport *report, const char *path,
                      DiagOutputFormat fmt);

#endif /* PLC_DIAGNOSTICS_H */
