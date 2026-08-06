/**
 * diagnostics.c - Advanced lint and code quality analysis.
 */

#include "diagnostics.h"
#include "plc_compiler.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ASTNode *node;
    char signature[256];
} ConditionRecord;

static void diag_add(DiagReport *r, DiagSeverity sev, DiagCategory cat,
                     int line, const char *msg, const char *sug) {
    if (!r || r->issue_count >= MAX_DIAG_ISSUES) return;

    DiagIssue *d = &r->issues[r->issue_count++];
    memset(d, 0, sizeof(*d));
    d->severity = sev;
    d->category = cat;
    d->line = line;
    strncpy(d->message, msg ? msg : "", MAX_DIAG_MSG - 1);
    strncpy(d->suggestion, sug ? sug : "", MAX_DIAG_MSG - 1);

    switch (sev) {
        case DIAG_HINT:  r->hint_count++; break;
        case DIAG_INFO:  r->info_count++; break;
        case DIAG_WARN:  r->warn_count++; break;
        case DIAG_ERROR: r->error_count++; break;
    }
}

static void text_append(char *out, int max, const char *text) {
    size_t len;
    size_t room;
    if (!out || !text || max <= 0) return;
    len = strlen(out);
    if (len >= (size_t)max - 1) return;
    room = (size_t)max - len - 1;
    strncat(out, text, room);
}

static const char *cmp_name(CmpOp op) {
    switch (op) {
        case CMP_EQ:  return "=";
        case CMP_NEQ: return "!=";
        case CMP_GTE: return ">=";
        case CMP_LTE: return "<=";
        case CMP_GT:  return ">";
        case CMP_LT:  return "<";
        default:      return "?";
    }
}

static void condition_signature(ASTNode *node, char *out, int max) {
    if (!node || !out || max <= 0) return;

    switch (node->type) {
        case NODE_COMPARISON:
            text_append(out, max, node->var_name);
            text_append(out, max, cmp_name(node->cmp_op));
            text_append(out, max, node->value);
            break;
        case NODE_AND:
            text_append(out, max, "(");
            condition_signature(node->children[0], out, max);
            text_append(out, max, " AND ");
            condition_signature(node->children[1], out, max);
            text_append(out, max, ")");
            break;
        case NODE_OR:
            text_append(out, max, "(");
            condition_signature(node->children[0], out, max);
            text_append(out, max, " OR ");
            condition_signature(node->children[1], out, max);
            text_append(out, max, ")");
            break;
        case NODE_NOT:
            text_append(out, max, "NOT(");
            condition_signature(node->children[0], out, max);
            text_append(out, max, ")");
            break;
        default:
            text_append(out, max, "?");
            break;
    }
}

static void collect_if_conditions(ASTNode *node, ConditionRecord *records,
                                  int *count, int max) {
    if (!node || !records || !count) return;

    if (node->type == NODE_IF && node->child_count > 0 && *count < max) {
        records[*count].node = node;
        records[*count].signature[0] = '\0';
        condition_signature(node->children[0], records[*count].signature,
                            sizeof(records[*count].signature));
        (*count)++;
    }

    for (int i = 0; i < node->child_count; i++)
        collect_if_conditions(node->children[i], records, count, max);

    for (ASTNode *next = node->next; next; next = next->next)
        collect_if_conditions(next, records, count, max);
}

static void check_duplicate_conditions(DiagReport *r, CompilerCtx *ctx) {
    ConditionRecord records[128];
    int count = 0;
    if (!ctx || !ctx->ast_root) return;

    collect_if_conditions(ctx->ast_root, records, &count, 128);
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (records[i].signature[0] &&
                strcmp(records[i].signature, records[j].signature) == 0) {
                char msg[MAX_DIAG_MSG];
                snprintf(msg, sizeof(msg),
                         "IF at line %d duplicates the condition from line %d",
                         records[j].node->line, records[i].node->line);
                diag_add(r, DIAG_INFO, DIAG_DUPLICATE, records[j].node->line,
                         msg, "Merge duplicate rules or document the intended priority");
            }
        }
    }
}

static void check_naming(DiagReport *r, CompilerCtx *ctx) {
    for (int i = 0; i < ctx->sym_count; i++) {
        Symbol *s = &ctx->symbols[i];
        int has_upper = 0;
        int has_lower = 0;

        if (s->direction == IO_OUTPUT && strlen(s->name) == 1) {
            char msg[MAX_DIAG_MSG];
            char sug[MAX_DIAG_MSG];
            snprintf(msg, sizeof(msg),
                     "Output variable '%s' has a single-character name", s->name);
            snprintf(sug, sizeof(sug),
                     "Use descriptive output names, for example '%s_out'", s->name);
            diag_add(r, DIAG_WARN, DIAG_NAMING, 0, msg, sug);
        }

        for (const char *p = s->name; *p; p++) {
            if (isupper((unsigned char)*p)) has_upper = 1;
            if (islower((unsigned char)*p)) has_lower = 1;
        }
        if (has_upper && has_lower && strlen(s->name) > 2) {
            char msg[MAX_DIAG_MSG];
            snprintf(msg, sizeof(msg),
                     "Variable '%s' mixes upper and lower case", s->name);
            diag_add(r, DIAG_HINT, DIAG_NAMING, 0, msg,
                     "Use snake_case or SCREAMING_SNAKE_CASE consistently");
        }
    }
}

static void check_unused_symbols(DiagReport *r, CompilerCtx *ctx) {
    for (int i = 0; i < ctx->sym_count; i++) {
        Symbol *s = &ctx->symbols[i];
        if (!s->is_used) {
            char msg[MAX_DIAG_MSG];
            snprintf(msg, sizeof(msg),
                     "Symbol '%s' is declared but never used", s->name);
            r->metrics.unused_symbols++;
            diag_add(r, DIAG_WARN, DIAG_UNUSED_SYMBOL, 0, msg,
                     "Remove unused symbols to keep the I/O table tight");
        }
    }
}

static int is_interesting_number(ASTNode *node) {
    if (!node || !node->is_numeric) return 0;
    return node->numeric_val != 0.0 && node->numeric_val != 1.0;
}

static void walk_ast_metrics(ASTNode *node, DiagReport *r, int depth) {
    if (!node) return;

    r->metrics.total_nodes++;
    if (depth > r->metrics.max_nesting_depth)
        r->metrics.max_nesting_depth = depth;

    switch (node->type) {
        case NODE_IF:
            r->metrics.if_count++;
            r->metrics.cyclomatic_complexity++;
            if (depth > 5) {
                char msg[MAX_DIAG_MSG];
                snprintf(msg, sizeof(msg),
                         "IF at line %d is nested %d levels deep",
                         node->line, depth);
                diag_add(r, DIAG_WARN, DIAG_NESTING_DEPTH, node->line, msg,
                         "Refactor deeply nested logic into function blocks or states");
            }
            break;
        case NODE_WHILE:
            r->metrics.while_count++;
            r->metrics.cyclomatic_complexity++;
            break;
        case NODE_CASE:
            r->metrics.case_count++;
            r->metrics.cyclomatic_complexity += node->child_count > 0 ? node->child_count : 1;
            break;
        case NODE_FUNCTION_BLOCK:
            r->metrics.function_block_count++;
            break;
        case NODE_STATE_MACHINE:
            r->metrics.state_machine_count++;
            break;
        case NODE_ASSERT:
            r->metrics.assert_count++;
            break;
        case NODE_COMPARISON:
        case NODE_ACTION:
            if (is_interesting_number(node)) {
                char msg[MAX_DIAG_MSG];
                snprintf(msg, sizeof(msg),
                         "Numeric literal %.4g appears at line %d",
                         node->numeric_val, node->line);
                diag_add(r, DIAG_HINT, DIAG_MAGIC_NUMBER, node->line, msg,
                         "Consider naming important thresholds in project docs");
            }
            break;
        default:
            break;
    }

    for (int i = 0; i < node->child_count; i++)
        walk_ast_metrics(node->children[i], r, depth + 1);

    for (ASTNode *next = node->next; next; next = next->next)
        walk_ast_metrics(next, r, depth);
}

static void check_complexity(DiagReport *r) {
    if (r->metrics.cyclomatic_complexity > 20) {
        char msg[MAX_DIAG_MSG];
        snprintf(msg, sizeof(msg),
                 "Cyclomatic complexity is %d, recommended maximum is 20",
                 r->metrics.cyclomatic_complexity);
        diag_add(r, DIAG_WARN, DIAG_COMPLEXITY, 0, msg,
                 "Split complex logic into function blocks or state machines");
    } else if (r->metrics.cyclomatic_complexity > 10) {
        char msg[MAX_DIAG_MSG];
        snprintf(msg, sizeof(msg),
                 "Cyclomatic complexity is %d, target maximum is 10",
                 r->metrics.cyclomatic_complexity);
        diag_add(r, DIAG_INFO, DIAG_COMPLEXITY, 0, msg,
                 "Consider decomposing the program into smaller units");
    }

    if (r->metrics.max_nesting_depth > 8) {
        char msg[MAX_DIAG_MSG];
        snprintf(msg, sizeof(msg),
                 "Maximum nesting depth is %d levels, recommended maximum is 6",
                 r->metrics.max_nesting_depth);
        diag_add(r, DIAG_WARN, DIAG_NESTING_DEPTH, 0, msg,
                 "Flatten deeply nested logic where practical");
    }

    if (r->metrics.total_nodes > 5000) {
        char msg[MAX_DIAG_MSG];
        snprintf(msg, sizeof(msg),
                 "Program has %d AST nodes", r->metrics.total_nodes);
        diag_add(r, DIAG_INFO, DIAG_LARGE_PROGRAM, 0, msg,
                 "Large programs may increase scan time and review effort");
    }
}

static int count_source_lines(const char *source) {
    int lines = 0;
    if (!source || !source[0]) return 0;
    for (const char *p = source; *p; p++)
        if (*p == '\n') lines++;
    return lines + 1;
}

static int compute_quality_score(DiagReport *r) {
    int score = 100;
    score -= r->error_count * 20;
    score -= r->warn_count * 5;
    score -= r->info_count * 2;
    score -= r->hint_count;
    score -= r->metrics.unused_symbols * 3;
    if (r->metrics.cyclomatic_complexity > 20) score -= 10;
    if (r->metrics.max_nesting_depth > 8) score -= 5;
    if (score < 0) score = 0;
    if (score > 100) score = 100;
    return score;
}

int diag_run(CompilerCtx *ctx, DiagReport *report) {
    if (!ctx || !report) return 0;
    memset(report, 0, sizeof(*report));

    report->metrics.total_symbols = ctx->sym_count;
    report->metrics.source_lines = count_source_lines(ctx->source);
    report->metrics.cyclomatic_complexity = 1;

    for (int i = 0; i < ctx->sym_count; i++) {
        Symbol *s = &ctx->symbols[i];
        if (s->direction == IO_INPUT) report->metrics.input_symbols++;
        if (s->direction == IO_OUTPUT) report->metrics.output_symbols++;
        if (s->kind == SYM_TIMER) report->metrics.timer_symbols++;
    }

    check_naming(report, ctx);
    check_unused_symbols(report, ctx);
    if (ctx->ast_root) walk_ast_metrics(ctx->ast_root, report, 0);
    check_duplicate_conditions(report, ctx);
    check_complexity(report);
    report->quality_score = compute_quality_score(report);
    return 1;
}

static const char *diag_sev_str(DiagSeverity s) {
    switch (s) {
        case DIAG_HINT:  return "HINT";
        case DIAG_INFO:  return "INFO";
        case DIAG_WARN:  return "WARN";
        case DIAG_ERROR: return "ERROR";
        default:         return "?";
    }
}

static const char *diag_cat_str(DiagCategory c) {
    switch (c) {
        case DIAG_DEAD_CODE:     return "dead_code";
        case DIAG_COMPLEXITY:    return "complexity";
        case DIAG_NAMING:        return "naming";
        case DIAG_DEPRECATED:    return "deprecated";
        case DIAG_DUPLICATE:     return "duplicate";
        case DIAG_UNUSED_SYMBOL: return "unused_symbol";
        case DIAG_NESTING_DEPTH: return "nesting_depth";
        case DIAG_LARGE_PROGRAM: return "large_program";
        case DIAG_NO_COMMENT:    return "no_comment";
        case DIAG_MAGIC_NUMBER:  return "magic_number";
        default:                 return "general";
    }
}

static void json_escape(const char *in, FILE *f) {
    for (const unsigned char *p = (const unsigned char *)in; p && *p; p++) {
        switch (*p) {
            case '\"': fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\b': fputs("\\b", f); break;
            case '\f': fputs("\\f", f); break;
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

static void render_text(const DiagReport *r, FILE *f) {
    fprintf(f, "\nPLC COMPILER DIAGNOSTICS REPORT\n");
    fprintf(f, "================================\n\n");
    fprintf(f, "Quality score: %d / 100\n\n", r->quality_score);
    fprintf(f, "Metrics:\n");
    fprintf(f, "  Source lines          : %d\n", r->metrics.source_lines);
    fprintf(f, "  Total AST nodes       : %d\n", r->metrics.total_nodes);
    fprintf(f, "  Cyclomatic complexity : %d\n", r->metrics.cyclomatic_complexity);
    fprintf(f, "  Max nesting depth     : %d\n", r->metrics.max_nesting_depth);
    fprintf(f, "  IF / WHILE / CASE     : %d / %d / %d\n",
            r->metrics.if_count, r->metrics.while_count, r->metrics.case_count);
    fprintf(f, "  Function blocks       : %d\n", r->metrics.function_block_count);
    fprintf(f, "  State machines        : %d\n", r->metrics.state_machine_count);
    fprintf(f, "  Assertions            : %d\n", r->metrics.assert_count);
    fprintf(f, "  Symbols               : %d (input:%d output:%d timer:%d unused:%d)\n\n",
            r->metrics.total_symbols, r->metrics.input_symbols,
            r->metrics.output_symbols, r->metrics.timer_symbols,
            r->metrics.unused_symbols);
    fprintf(f, "Issues: errors:%d warnings:%d info:%d hints:%d\n",
            r->error_count, r->warn_count, r->info_count, r->hint_count);
    fprintf(f, "----------------------------------------------------------------\n");

    for (int i = 0; i < r->issue_count; i++) {
        const DiagIssue *d = &r->issues[i];
        fprintf(f, "[%s] (%s)", diag_sev_str(d->severity), diag_cat_str(d->category));
        if (d->line > 0) fprintf(f, " line %d", d->line);
        fprintf(f, "\n  %s\n  Suggestion: %s\n\n", d->message, d->suggestion);
    }
}

static void render_json(const DiagReport *r, FILE *f) {
    fprintf(f, "{\n  \"quality_score\": %d,\n", r->quality_score);
    fprintf(f, "  \"metrics\": {\n");
    fprintf(f, "    \"source_lines\": %d,\n", r->metrics.source_lines);
    fprintf(f, "    \"total_nodes\": %d,\n", r->metrics.total_nodes);
    fprintf(f, "    \"cyclomatic_complexity\": %d,\n", r->metrics.cyclomatic_complexity);
    fprintf(f, "    \"max_nesting_depth\": %d,\n", r->metrics.max_nesting_depth);
    fprintf(f, "    \"if_count\": %d,\n", r->metrics.if_count);
    fprintf(f, "    \"while_count\": %d,\n", r->metrics.while_count);
    fprintf(f, "    \"case_count\": %d,\n", r->metrics.case_count);
    fprintf(f, "    \"function_block_count\": %d,\n", r->metrics.function_block_count);
    fprintf(f, "    \"state_machine_count\": %d,\n", r->metrics.state_machine_count);
    fprintf(f, "    \"assert_count\": %d,\n", r->metrics.assert_count);
    fprintf(f, "    \"total_symbols\": %d,\n", r->metrics.total_symbols);
    fprintf(f, "    \"input_symbols\": %d,\n", r->metrics.input_symbols);
    fprintf(f, "    \"output_symbols\": %d,\n", r->metrics.output_symbols);
    fprintf(f, "    \"timer_symbols\": %d,\n", r->metrics.timer_symbols);
    fprintf(f, "    \"unused_symbols\": %d\n  },\n", r->metrics.unused_symbols);
    fprintf(f, "  \"summary\": { \"errors\": %d, \"warnings\": %d, \"info\": %d, \"hints\": %d },\n",
            r->error_count, r->warn_count, r->info_count, r->hint_count);
    fprintf(f, "  \"issues\": [\n");
    for (int i = 0; i < r->issue_count; i++) {
        const DiagIssue *d = &r->issues[i];
        if (i) fprintf(f, ",\n");
        fprintf(f, "    { \"severity\": \"%s\", \"category\": \"%s\", \"line\": %d, \"message\": \"",
                diag_sev_str(d->severity), diag_cat_str(d->category), d->line);
        json_escape(d->message, f);
        fprintf(f, "\", \"suggestion\": \"");
        json_escape(d->suggestion, f);
        fprintf(f, "\" }");
    }
    fprintf(f, "\n  ]\n}\n");
}

static void render_markdown(const DiagReport *r, FILE *f) {
    fprintf(f, "# PLC Compiler Diagnostics Report\n\n");
    fprintf(f, "**Quality Score:** %d / 100\n\n", r->quality_score);
    fprintf(f, "## Metrics\n\n");
    fprintf(f, "| Metric | Value |\n|--------|-------|\n");
    fprintf(f, "| Source Lines | %d |\n", r->metrics.source_lines);
    fprintf(f, "| Cyclomatic Complexity | %d |\n", r->metrics.cyclomatic_complexity);
    fprintf(f, "| Max Nesting Depth | %d |\n", r->metrics.max_nesting_depth);
    fprintf(f, "| Total AST Nodes | %d |\n", r->metrics.total_nodes);
    fprintf(f, "| IF / WHILE / CASE | %d / %d / %d |\n",
            r->metrics.if_count, r->metrics.while_count, r->metrics.case_count);
    fprintf(f, "| Total Symbols | %d |\n", r->metrics.total_symbols);
    fprintf(f, "| Unused Symbols | %d |\n\n", r->metrics.unused_symbols);
    fprintf(f, "## Issues\n\n");
    fprintf(f, "| Severity | Category | Line | Message | Suggestion |\n");
    fprintf(f, "|----------|----------|------|---------|------------|\n");
    for (int i = 0; i < r->issue_count; i++) {
        const DiagIssue *d = &r->issues[i];
        fprintf(f, "| %s | %s | %d | %s | %s |\n",
                diag_sev_str(d->severity), diag_cat_str(d->category),
                d->line, d->message, d->suggestion);
    }
}

static void render_html(const DiagReport *r, FILE *f) {
    fprintf(f,
            "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            "<title>PLC Compiler Diagnostics</title>"
            "<style>body{font-family:system-ui,Arial,sans-serif;margin:2rem;color:#1b1f24}"
            "table{border-collapse:collapse;width:100%%;margin:1rem 0}"
            "th,td{border:1px solid #d0d7de;padding:.5rem;text-align:left}"
            "th{background:#f6f8fa}.score{font-size:2rem;font-weight:700}"
            ".ERROR{color:#b42318}.WARN{color:#b54708}.INFO{color:#175cd3}"
            ".HINT{color:#57606a}</style></head><body>");
    fprintf(f, "<h1>PLC Compiler Diagnostics</h1>");
    fprintf(f, "<p class=\"score\">Quality Score: %d / 100</p>", r->quality_score);
    fprintf(f, "<h2>Metrics</h2><table><tr><th>Metric</th><th>Value</th></tr>");
    fprintf(f, "<tr><td>Source Lines</td><td>%d</td></tr>", r->metrics.source_lines);
    fprintf(f, "<tr><td>Cyclomatic Complexity</td><td>%d</td></tr>", r->metrics.cyclomatic_complexity);
    fprintf(f, "<tr><td>Max Nesting Depth</td><td>%d</td></tr>", r->metrics.max_nesting_depth);
    fprintf(f, "<tr><td>Total AST Nodes</td><td>%d</td></tr>", r->metrics.total_nodes);
    fprintf(f, "<tr><td>Total Symbols</td><td>%d</td></tr>", r->metrics.total_symbols);
    fprintf(f, "<tr><td>Unused Symbols</td><td>%d</td></tr></table>", r->metrics.unused_symbols);
    fprintf(f, "<h2>Issues</h2><table><tr><th>Severity</th><th>Category</th><th>Line</th><th>Message</th><th>Suggestion</th></tr>");
    for (int i = 0; i < r->issue_count; i++) {
        const DiagIssue *d = &r->issues[i];
        const char *sev = diag_sev_str(d->severity);
        fprintf(f, "<tr><td class=\"%s\">%s</td><td>%s</td><td>%d</td><td>%s</td><td>%s</td></tr>",
                sev, sev, diag_cat_str(d->category), d->line,
                d->message, d->suggestion);
    }
    fprintf(f, "</table></body></html>\n");
}

void diag_print_report(const DiagReport *report, DiagOutputFormat fmt) {
    if (!report) return;
    switch (fmt) {
        case DIAG_FMT_JSON:     render_json(report, stdout); break;
        case DIAG_FMT_MARKDOWN: render_markdown(report, stdout); break;
        case DIAG_FMT_HTML:     render_html(report, stdout); break;
        default:                render_text(report, stdout); break;
    }
}

int diag_write_report(const DiagReport *report, const char *path,
                      DiagOutputFormat fmt) {
    FILE *f;
    if (!report || !path) return 0;
    f = fopen(path, "w");
    if (!f) return 0;

    switch (fmt) {
        case DIAG_FMT_JSON:     render_json(report, f); break;
        case DIAG_FMT_MARKDOWN: render_markdown(report, f); break;
        case DIAG_FMT_HTML:     render_html(report, f); break;
        default:                render_text(report, f); break;
    }

    fclose(f);
    return 1;
}
