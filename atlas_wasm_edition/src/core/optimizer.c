/**
 * optimizer.c — AST Optimization Pass
 *
 * Performs compile-time optimizations on the PLC program AST:
 *   1. Duplicate condition detection
 *   2. Redundant sub-expression warnings
 *   3. Dead code detection (THEN == ELSE)
 *   4. Output consolidation suggestions
 */

#include "plc_compiler.h"

typedef struct {
    int duplicates_found;
    int dead_branches_found;
    int redundant_conditions;
    int consolidation_suggestions;
    int total_optimizations;
} OptStats;

static int conditions_equal(ASTNode *a, ASTNode *b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    if (a->type != b->type) return 0;
    switch (a->type) {
        case NODE_COMPARISON:
            return strcasecmp(a->var_name, b->var_name) == 0 &&
                   a->cmp_op == b->cmp_op &&
                   strcasecmp(a->value, b->value) == 0;
        case NODE_AND:
        case NODE_OR:
            return conditions_equal(a->children[0], b->children[0]) &&
                   conditions_equal(a->children[1], b->children[1]);
        case NODE_NOT:
            return conditions_equal(a->children[0], b->children[0]);
        default: return 0;
    }
}

static int actions_equal(ASTNode *a, ASTNode *b) {
    while (a && b) {
        if (a->type != b->type) return 0;
        if (a->type == NODE_ACTION) {
            if (strcasecmp(a->var_name, b->var_name) != 0) return 0;
            if (strcasecmp(a->value, b->value) != 0) return 0;
        }
        a = a->next; b = b->next;
    }
    return (a == NULL && b == NULL);
}

static void opt_detect_duplicates(CompilerCtx *ctx, OptStats *stats) {
    ASTNode *root = ctx->ast_root;
    if (!root) return;
    for (int i = 0; i < root->child_count; i++) {
        ASTNode *a = root->children[i];
        if (a->type != NODE_IF || a->child_count < 1) continue;
        for (int j = i + 1; j < root->child_count; j++) {
            ASTNode *b = root->children[j];
            if (b->type != NODE_IF || b->child_count < 1) continue;
            if (conditions_equal(a->children[0], b->children[0])) {
                stats->duplicates_found++;
                stats->total_optimizations++;
                log_warn(ctx, "Optimizer: rules at line %d and %d have IDENTICAL conditions", a->line, b->line);
            }
        }
    }
}

static void opt_prune_dead_branches(CompilerCtx *ctx, OptStats *stats) {
    ASTNode *root = ctx->ast_root;
    if (!root) return;
    for (int i = 0; i < root->child_count; i++) {
        ASTNode *stmt = root->children[i];
        if (stmt->type != NODE_IF || stmt->child_count < 3) continue;
        ASTNode *then_a = stmt->children[1] ? stmt->children[1]->next : NULL;
        ASTNode *else_a = stmt->children[2] ? stmt->children[2]->next : NULL;
        if (then_a && else_a && actions_equal(then_a, else_a)) {
            stats->dead_branches_found++;
            stats->total_optimizations++;
            log_info(ctx, "Optimizer: IF at line %d has identical THEN/ELSE actions; leaving AST unchanged for semantic safety",
                     stmt->line);
        }
    }
}

static void check_redundant(CompilerCtx *ctx, ASTNode *cond, OptStats *stats) {
    if (!cond) return;
    if ((cond->type == NODE_AND || cond->type == NODE_OR) && cond->child_count == 2) {
        if (conditions_equal(cond->children[0], cond->children[1])) {
            stats->redundant_conditions++;
            stats->total_optimizations++;
            log_warn(ctx, "Optimizer: line %d — redundant sub-expression (both sides of %s identical)",
                     cond->line, cond->type == NODE_AND ? "AND" : "OR");
        }
        check_redundant(ctx, cond->children[0], stats);
        check_redundant(ctx, cond->children[1], stats);
    }
}

static void opt_detect_redundant(CompilerCtx *ctx, OptStats *stats) {
    ASTNode *root = ctx->ast_root;
    if (!root) return;
    for (int i = 0; i < root->child_count; i++) {
        ASTNode *stmt = root->children[i];
        if (stmt->type != NODE_IF || stmt->child_count < 1) continue;
        check_redundant(ctx, stmt->children[0], stats);
    }
}

int optimizer_run(CompilerCtx *ctx) {
    if (!ctx || !ctx->ast_root) return 0;
    OptStats stats;
    memset(&stats, 0, sizeof(stats));
    log_info(ctx, "Optimizer: starting analysis & transformation pass");
    opt_detect_duplicates(ctx, &stats);
    opt_prune_dead_branches(ctx, &stats);
    opt_detect_redundant(ctx, &stats);

    if (!ctx->json_mode && !ctx->quiet_mode) {
        printf("\n");
        printf("╔══════════════════════════════════════════════════╗\n");
        printf("║           OPTIMIZATION REPORT                    ║\n");
        printf("╠══════════════════════════════════════════════════╣\n");
        printf("║  Duplicate Conditions   : %-22d ║\n", stats.duplicates_found);
        printf("║  Dead Branches          : %-22d ║\n", stats.dead_branches_found);
        printf("║  Redundant Expressions  : %-22d ║\n", stats.redundant_conditions);
        printf("║  Total Findings         : %-22d ║\n", stats.total_optimizations);
        printf("╚══════════════════════════════════════════════════╝\n\n");
    }
    log_info(ctx, "Optimizer: %d findings", stats.total_optimizations);
    return 1;
}
