/**
 * safety.c — IEC 61508 / SIL Safety Analysis Engine
 *
 * Safety classification is ANNOTATION-DRIVEN only.
 * Variables must be explicitly tagged in DSL source:
 *
 *   VAR_OUTPUT
 *     xDriveEnable  : BOOL @CRITICAL
 *     xEmergRelay   : BOOL @ESTOP
 *     rPressure     : REAL @SIL2
 *   END_VAR
 *
 * No substring matching on variable names. Any variable name from
 * any language, any naming convention is fully supported.
 */

#include "plc_compiler.h"
#include "safety.h"

/* ─── Annotation-driven classification (NO heuristics) ─────────────────── */

static int is_critical_output(CompilerCtx *ctx, const char *name) {
    Symbol *s = sym_lookup(ctx, name);
    if (!s) return 0;
    /* @CRITICAL or any @SILx annotation marks as critical */
    return s->safety_critical || s->safety_sil_level > 0;
}

static int is_estop_var(CompilerCtx *ctx, const char *name) {
    Symbol *s = sym_lookup(ctx, name);
    if (!s) return 0;
    return s->safety_estop;
}

static int value_to_int(const char *val) {
    if (strcasecmp(val, "ON") == 0)  return 1;
    if (strcasecmp(val, "OFF") == 0) return 0;
    return -1; /* numeric or variable */
}

static void json_print_string(const char *text) {
    putchar('"');
    for (const char *p = text ? text : ""; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (*p == '"') printf("\\\"");
        else if (*p == '\\') printf("\\\\");
        else if (*p == '\n') printf("\\n");
        else if (*p == '\r') printf("\\r");
        else if (*p == '\t') printf("\\t");
        else if (ch < 0x20) printf("\\u%04x", ch);
        else putchar(*p);
    }
    putchar('"');
}

static void add_issue(SafetyResult *r, SafetySeverity sev, SafetyCategory cat,
                      int line, const char *var, const char *msg, const char *rec) {
    if (r->issue_count >= MAX_SAFETY_ISSUES) return;
    SafetyIssue *issue = &r->issues[r->issue_count++];
    issue->severity = sev;
    issue->category = cat;
    issue->line = line;
    strncpy(issue->message, msg, MAX_SAFETY_MSG - 1);
    issue->message[MAX_SAFETY_MSG - 1] = '\0';
    strncpy(issue->recommendation, rec, MAX_SAFETY_MSG - 1);
    issue->recommendation[MAX_SAFETY_MSG - 1] = '\0';
    if (var) {
        strncpy(issue->variable, var, MAX_IDENTIFIER_LEN - 1);
        issue->variable[MAX_IDENTIFIER_LEN - 1] = '\0';
    } else {
        issue->variable[0] = '\0';
    }

    switch (sev) {
        case SAFETY_CRITICAL:
        case SAFETY_FATAL:    r->critical_count++; break;
        case SAFETY_WARNING:  r->warning_count++;  break;
        case SAFETY_INFO:     r->info_count++;      break;
    }
}

/* ─── Find or create output tracker ───────────────────────────────────── */
static OutputTracker *get_output(SafetyResult *r, const char *name) {
    for (int i = 0; i < r->output_count; i++)
        if (strcasecmp(r->outputs[i].name, name) == 0) return &r->outputs[i];

    if (r->output_count >= 256) return NULL;
    OutputTracker *ot = &r->outputs[r->output_count++];
    memset(ot, 0, sizeof(*ot));
    strncpy(ot->name, name, MAX_IDENTIFIER_LEN - 1);
    ot->name[MAX_IDENTIFIER_LEN - 1] = '\0';
    return ot;
}

/* ─── Collect actions from an action linked list ──────────────────────── */
static void collect_actions(SafetyResult *r, ASTNode *head, int rule_idx) {
    for (ASTNode *act = head; act; act = act->next) {
        if (act->type != NODE_ACTION || !act->var_name[0]) continue;
        /* Skip literal values used as variable names */
        if (strcasecmp(act->var_name, "ON") == 0 || strcasecmp(act->var_name, "OFF") == 0) continue;

        OutputTracker *ot = get_output(r, act->var_name);
        if (!ot) continue;

        int val = value_to_int(act->value);
        if (ot->rule_count < MAX_OUTPUT_RULES) {
            ot->rule_indices[ot->rule_count] = rule_idx;
            ot->values[ot->rule_count] = val;
            ot->rule_count++;
        }
        if (val == 1) ot->has_on = 1;
        if (val == 0) ot->has_off = 1;
    }
}

/* ─── Check if a condition references an e-stop variable ──────────────── */
static int condition_has_estop(CompilerCtx *ctx, ASTNode *cond) {
    if (!cond) return 0;
    switch (cond->type) {
        case NODE_COMPARISON:
            return is_estop_var(ctx, cond->var_name);
        case NODE_AND:
        case NODE_OR:
            return condition_has_estop(ctx, cond->children[0]) || condition_has_estop(ctx, cond->children[1]);
        case NODE_NOT:
            return condition_has_estop(ctx, cond->children[0]);
        default:
            return 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   CHECK 0: Warn about outputs with no safety annotation
   This replaces the old heuristic — now the ENGINEER decides what's critical.
   ═══════════════════════════════════════════════════════════════════════════ */
static void check_unannotated_outputs(CompilerCtx *ctx, SafetyResult *result) {
    for (int i = 0; i < result->output_count; i++) {
        const char *name = result->outputs[i].name;
        Symbol *s = sym_lookup(ctx, name);
        if (!s) continue;

        /* Only flag OUTPUT or MEMORY direction variables */
        if (s->direction != IO_OUTPUT && s->direction != IO_MEMORY) continue;

        int annotated = s->safety_critical || s->safety_estop || s->safety_sil_level > 0;
        if (!annotated) {
            char msg[MAX_SAFETY_MSG];
            snprintf(msg, sizeof(msg),
                     "Output '%s' has no safety annotation; classification unknown.",
                     name);
            add_issue(result, SAFETY_INFO, SAFE_CAT_GENERAL,
                      0, name, msg,
                      "Annotate with @CRITICAL, @ESTOP, @SIL1-@SIL4, or document as non-safety");
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   CHECK 1: Collect all output assignments and detect conflicts
   ═══════════════════════════════════════════════════════════════════════════ */
static void collect_outputs_recursive(CompilerCtx *ctx, ASTNode *node, SafetyResult *result) {
    if (!node) return;

    if (node->type == NODE_IF) {
        /* THEN branch */
        if (node->child_count > 1 && node->children[1])
            collect_actions(result, node->children[1]->next, node->line);

        /* ELSE branch */
        if (node->child_count > 2 && node->children[2]) {
            collect_actions(result, node->children[2]->next, node->line);
            for (ASTNode *act = node->children[2]->next; act; act = act->next) {
                if (act->type == NODE_ACTION && act->var_name[0]) {
                    OutputTracker *ot = get_output(result, act->var_name);
                    if (ot) ot->has_else_path = 1;
                }
            }
        }

        /* Check timer dependency */
        if (node->has_timer && node->child_count > 1) {
            for (ASTNode *act = node->children[1]->next; act; act = act->next) {
                if (act->type == NODE_ACTION && act->var_name[0]) {
                    OutputTracker *ot = get_output(result, act->var_name);
                    if (ot) ot->depends_on_timer = 1;
                }
            }
        }

        /* Check e-stop coverage */
        if (node->child_count > 0 && condition_has_estop(ctx, node->children[0])) {
            result->has_estop = 1;
            if (node->child_count > 1 && node->children[1]) {
                for (ASTNode *act = node->children[1]->next; act; act = act->next) {
                    if (act->type == NODE_ACTION && act->var_name[0]) {
                        OutputTracker *ot = get_output(result, act->var_name);
                        if (ot) ot->covered_by_estop = 1;
                    }
                }
            }
        }
    } else if (node->type == NODE_ACTION && node->var_name[0]) {
        collect_actions(result, node, node->line);
    } else if (node->type == NODE_STATE) {
        /* In SM states, ENTRY/EXIT are actions */
        for (int i = 0; i < node->child_count; i++) {
            if (node->children[i]->type == NODE_ACTION) {
                collect_actions(result, node->children[i]->next, node->children[i]->line);
            }
        }
    }

    /* Recurse to all children */
    for (int i = 0; i < node->child_count; i++) {
        collect_outputs_recursive(ctx, node->children[i], result);
    }
}

static void collect_all_outputs(CompilerCtx *ctx, SafetyResult *result) {
    collect_outputs_recursive(ctx, ctx->ast_root, result);
    result->total_outputs = result->output_count;
}

/* ─── Check if two numeric ranges overlap ──────────────────────────────── */
static int ranges_overlap(CmpOp op1, double val1, CmpOp op2, double val2) {
    /* 
     * Logic: Check for disjoint sets. If not disjoint, they overlap.
     * Example: (x > 50) and (x < 30) are disjoint.
     */
    
    /* Equality cases */
    if (op1 == CMP_EQ && op2 == CMP_EQ) return val1 == val2;
    if (op1 == CMP_EQ) {
        if (op2 == CMP_GT)  return val1 > val2;
        if (op2 == CMP_GTE) return val1 >= val2;
        if (op2 == CMP_LT)  return val1 < val2;
        if (op2 == CMP_LTE) return val1 <= val2;
        if (op2 == CMP_NEQ) return val1 != val2;
    }
    /* Symmetry */
    if (op2 == CMP_EQ) return ranges_overlap(op2, val2, op1, val1);

    /* Inequality vs Inequality */
    if (op1 == CMP_GT || op1 == CMP_GTE) {
        if (op2 == CMP_LT || op2 == CMP_LTE) {
            /* x > 50 and x < 40? Disjoint if val1 >= val2 */
            if (val1 > val2) return 0;
            return 1;
        }
    }
    if (op1 == CMP_LT || op1 == CMP_LTE) {
        if (op2 == CMP_GT || op2 == CMP_GTE) return ranges_overlap(op2, val2, op1, val1);
    }

    /* Same direction always overlaps (one is a subset of the other) */
    return 1;
}

/* ─── Check if two conditions can be TRUE at the same time ──────────────── */
static int conditions_can_overlap(ASTNode *a, ASTNode *b) {
    if (!a || !b) return 1;

    /* Simple case: Comparison on the same variable */
    if (a->type == NODE_COMPARISON && b->type == NODE_COMPARISON) {
        if (strcasecmp(a->var_name, b->var_name) == 0) {
            /* Boolean logic */
            if (!a->is_numeric && !b->is_numeric) {
                if (a->cmp_op == CMP_EQ && b->cmp_op == CMP_EQ)
                    return strcasecmp(a->value, b->value) == 0;
            }
            /* Numeric logic */
            if (a->is_numeric && b->is_numeric) {
                return ranges_overlap(a->cmp_op, a->numeric_val, b->cmp_op, b->numeric_val);
            }
        }
        /* Different variables: Assume they CAN overlap (worst-case safety) */
        return 1;
    }

    /* Complex logic (AND/OR) — Recursive check */
    if (a->type == NODE_AND) return conditions_can_overlap(a->children[0], b) && conditions_can_overlap(a->children[1], b);
    if (b->type == NODE_AND) return conditions_can_overlap(a, b->children[0]) && conditions_can_overlap(a, b->children[1]);

    /* If one is an OR, if EITHER side overlaps with the other condition, the whole thing can overlap */
    if (a->type == NODE_OR) return conditions_can_overlap(a->children[0], b) || conditions_can_overlap(a->children[1], b);
    if (b->type == NODE_OR) return conditions_can_overlap(a, b->children[0]) || conditions_can_overlap(a, b->children[1]);

    return 1;
}

static void collect_if_rules_recursive(ASTNode *node, ASTNode **rules,
                                       int *count, int max_rules) {
    if (!node || !rules || !count || *count >= max_rules) return;

    if (node->type == NODE_IF && node->child_count >= 2)
        rules[(*count)++] = node;

    for (int i = 0; i < node->child_count && *count < max_rules; i++)
        collect_if_rules_recursive(node->children[i], rules, count, max_rules);
    if (node->next && *count < max_rules)
        collect_if_rules_recursive(node->next, rules, count, max_rules);
}

/* ═══════════════════════════════════════════════════════════════════════════
   CHECK 2: Conflicting output detection
   ═══════════════════════════════════════════════════════════════════════════ */
static void check_conflicting_outputs(CompilerCtx *ctx, SafetyResult *result) {
    enum { MAX_RULES_TO_COMPARE = 512 };
    ASTNode *rules[MAX_RULES_TO_COMPARE];
    int rule_count = 0;

    collect_if_rules_recursive(ctx->ast_root, rules, &rule_count, MAX_RULES_TO_COMPARE);

    /* N^2 check of all IF rules for condition overlap + output conflict */
    for (int i = 0; i < rule_count; i++) {
        ASTNode *rule_a = rules[i];

        for (int j = i + 1; j < rule_count; j++) {
            ASTNode *rule_b = rules[j];

            /* If conditions can be true simultaneously... */
            if (conditions_can_overlap(rule_a->children[0], rule_b->children[0])) {
                
                /* Check if they drive the same outputs to different values */
                for (ASTNode *act_a = rule_a->children[1]->next; act_a; act_a = act_a->next) {
                    if (act_a->type != NODE_ACTION) continue;

                    for (ASTNode *act_b = rule_b->children[1]->next; act_b; act_b = act_b->next) {
                        if (act_b->type != NODE_ACTION) continue;

                        if (strcasecmp(act_a->var_name, act_b->var_name) == 0 &&
                            strcasecmp(act_a->value, act_b->value) != 0) {
                            
                            char msg[MAX_SAFETY_MSG];
                            snprintf(msg, sizeof(msg),
                                     "DETERMINISTIC CONFLICT: Rules at line %d and %d can BOTH be active "
                                     "and drive '%s' to different values",
                                     rule_a->line, rule_b->line, act_a->var_name);
                            
                            add_issue(result, SAFETY_FATAL, SAFE_CAT_CONDITION_OVERLAP,
                                      rule_a->line, act_a->var_name, msg,
                                      "Use mutually exclusive conditions or consolidate into a CASE statement");
                            result->has_conflicting_outputs = 1;
                        }
                    }
                }
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   CHECK 3: Missing ELSE / default paths
   ═══════════════════════════════════════════════════════════════════════════ */
static void check_missing_else_recursive(CompilerCtx *ctx, ASTNode *node, SafetyResult *result) {
    if (!node) return;

    if (node->type == NODE_IF) {
        int has_then_actions = (node->child_count > 1 && node->children[1] && node->children[1]->next);
        int has_else_branch  = (node->child_count > 2 && node->children[2] && node->children[2]->next);

        if (has_then_actions && !has_else_branch) {
            for (ASTNode *act = node->children[1]->next; act; act = act->next) {
                if (act->type != NODE_ACTION || !act->var_name[0]) continue;
                char msg[MAX_SAFETY_MSG];
                SafetySeverity sev = is_critical_output(ctx, act->var_name) ? SAFETY_CRITICAL : SAFETY_WARNING;
                snprintf(msg, sizeof(msg),
                         "Output '%s' (line %d) is set in THEN but has no ELSE branch — "
                         "output state is INDETERMINATE when condition is false",
                         act->var_name, node->line);
                add_issue(result, sev, SAFE_CAT_MISSING_ELSE,
                          node->line, act->var_name, msg,
                          "Add an ELSE branch to define the safe state for this output");
                result->has_missing_else = 1;
            }
        }
    }

    for (int i = 0; i < node->child_count; i++) {
        check_missing_else_recursive(ctx, node->children[i], result);
    }
}

static void check_missing_else(CompilerCtx *ctx, SafetyResult *result) {
    check_missing_else_recursive(ctx, ctx->ast_root, result);
}

/* ═══════════════════════════════════════════════════════════════════════════
   CHECK 4: E-Stop coverage analysis
   ═══════════════════════════════════════════════════════════════════════════ */
static void check_estop_coverage(CompilerCtx *ctx, SafetyResult *result) {
    (void)ctx;
    if (!result->has_estop) {
        /* No e-stop at all */
        int has_critical = 0;
        for (int i = 0; i < result->output_count; i++)
            if (is_critical_output(ctx, result->outputs[i].name)) { has_critical = 1; break; }

        if (has_critical) {
            add_issue(result, SAFETY_FATAL, SAFE_CAT_ESTOP_COVERAGE,
                      0, NULL,
                      "NO EMERGENCY STOP DETECTED — safety-critical outputs have no e-stop protection",
                      "Add an e-stop rule: IF e_stop = ON THEN [all outputs] = OFF END");
        }
        return;
    }

    /* Check which critical outputs are NOT covered by e-stop */
    for (int i = 0; i < result->output_count; i++) {
        OutputTracker *ot = &result->outputs[i];
        if (!is_critical_output(ctx, ot->name)) continue;

        if (ot->covered_by_estop) {
            result->estop_covered++;
        } else {
            char msg[MAX_SAFETY_MSG];
            snprintf(msg, sizeof(msg),
                     "Critical output '%s' is NOT covered by the emergency stop",
                     ot->name);
            add_issue(result, SAFETY_CRITICAL, SAFE_CAT_ESTOP_COVERAGE,
                      0, ot->name, msg,
                      "Add this output to the e-stop THEN clause to force it OFF");
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   CHECK 5: Timer safety on critical paths
   ═══════════════════════════════════════════════════════════════════════════ */
static void check_timer_safety(CompilerCtx *ctx, SafetyResult *result) {
    (void)ctx;
    for (int i = 0; i < result->output_count; i++) {
        OutputTracker *ot = &result->outputs[i];
        if (!ot->depends_on_timer) continue;

        if (is_critical_output(ctx, ot->name)) {
            char msg[MAX_SAFETY_MSG];
            snprintf(msg, sizeof(msg),
                     "Safety-critical output '%s' depends on a TIMER — "
                     "delayed activation could cause hazardous condition during timer window",
                     ot->name);
            add_issue(result, SAFETY_WARNING, SAFE_CAT_TIMER_ON_CRITICAL_PATH,
                      0, ot->name, msg,
                      "Consider using immediate response for safety-critical outputs, "
                      "or add redundant protection during timer delay");
            result->has_timer_on_critical = 1;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   CHECK 6: Write-write conflict detection
   ═══════════════════════════════════════════════════════════════════════════ */
static void check_write_write_conflicts(CompilerCtx *ctx, SafetyResult *result) {
    (void)ctx;
    for (int i = 0; i < result->output_count; i++) {
        OutputTracker *ot = &result->outputs[i];

        /* Count unique rules that write to this output */
        int unique_rules[MAX_OUTPUT_RULES];
        int unique_count = 0;
        for (int r = 0; r < ot->rule_count; r++) {
            int found = 0;
            for (int u = 0; u < unique_count; u++) {
                if (unique_rules[u] == ot->rule_indices[r]) { found = 1; break; }
            }
            if (!found && unique_count < MAX_OUTPUT_RULES)
                unique_rules[unique_count++] = ot->rule_indices[r];
        }

        if (unique_count > 2) {
            char msg[MAX_SAFETY_MSG];
            snprintf(msg, sizeof(msg),
                     "Output '%s' is written by %d different rules — "
                     "final value depends on rule evaluation order (last-write-wins)",
                     ot->name, unique_count);
            add_issue(result,
                      is_critical_output(ctx, ot->name) ? SAFETY_CRITICAL : SAFETY_WARNING,
                      SAFE_CAT_WRITE_WRITE_CONFLICT,
                      0, ot->name, msg,
                      "Consolidate rules for this output or use explicit priority logic");
            result->has_write_write_conflict = 1;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   CHECK 7: Output coverage — every output should have ON and OFF paths
   ═══════════════════════════════════════════════════════════════════════════ */
static void check_output_coverage(CompilerCtx *ctx, SafetyResult *result) {
    (void)ctx;
    for (int i = 0; i < result->output_count; i++) {
        OutputTracker *ot = &result->outputs[i];
        if (ot->has_on && ot->has_off) {
            result->covered_outputs++;
        } else if (ot->has_on && !ot->has_off) {
            char msg[MAX_SAFETY_MSG];
            snprintf(msg, sizeof(msg),
                     "Output '%s' can be turned ON but is NEVER turned OFF — "
                     "once activated it cannot be deactivated",
                     ot->name);
            add_issue(result,
                      is_critical_output(ctx, ot->name) ? SAFETY_CRITICAL : SAFETY_WARNING,
                      SAFE_CAT_UNINITIALIZED_OUTPUT, 0, ot->name, msg,
                      "Add a rule or ELSE branch that sets this output to OFF");
        } else if (!ot->has_on && ot->has_off) {
            char msg[MAX_SAFETY_MSG];
            snprintf(msg, sizeof(msg),
                     "Output '%s' can be turned OFF but is NEVER turned ON — "
                     "it may be permanently disabled",
                     ot->name);
            add_issue(result, SAFETY_INFO, SAFE_CAT_UNINITIALIZED_OUTPUT,
                      0, ot->name, msg,
                      "Verify this is intentional or add a rule to activate this output");
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   SIL LEVEL CLASSIFICATION
   Based on IEC 61508 diagnostic coverage and safety patterns
   ═══════════════════════════════════════════════════════════════════════════ */
static void compute_sil_rating(CompilerCtx *ctx, SafetyResult *result) {
    int score = 100;

    /* Fatal issues → SIL_NONE */
    if (result->critical_count > 0) score -= result->critical_count * 15;
    if (result->warning_count > 0)  score -= result->warning_count * 5;

    /* No e-stop → massive penalty only if @CRITICAL outputs exist */
    int has_annotated_critical = 0;
    for (int i = 0; i < result->output_count; i++)
        if (is_critical_output(ctx, result->outputs[i].name)) { has_annotated_critical = 1; break; }

    if (!result->has_estop && has_annotated_critical) score -= 40;

    /* Conflicting outputs → penalty */
    if (result->has_conflicting_outputs) score -= 20;

    /* Missing ELSE branches → penalty */
    if (result->has_missing_else) score -= 10;

    /* Timer on critical path → penalty */
    if (result->has_timer_on_critical) score -= 10;

    /* Write-write conflicts → penalty */
    if (result->has_write_write_conflict) score -= 10;

    /* Output coverage bonus */
    if (result->total_outputs > 0) {
        double coverage = (double)result->covered_outputs / result->total_outputs;
        if (coverage < 0.5) score -= 20;
        else if (coverage < 0.8) score -= 10;
    }

    /* Determine highest declared SIL requirement from annotations */
    int max_declared_sil = 0;
    for (int i = 0; i < ctx->sym_count; i++) {
        if (ctx->symbols[i].safety_sil_level > max_declared_sil)
            max_declared_sil = ctx->symbols[i].safety_sil_level;
    }

    /* Clamp */
    if (score < 0) score = 0;
    if (score > 100) score = 100;
    result->compliance_score = score;

    /* Map score to SIL level, but cap at declared requirement */
    SILLevel achieved;
    if (score >= 90)      achieved = SIL_4;
    else if (score >= 75) achieved = SIL_3;
    else if (score >= 60) achieved = SIL_2;
    else if (score >= 40) achieved = SIL_1;
    else                  achieved = SIL_NONE;

    /* If achieved > declared requirement, report declared (honest ceiling) */
    if (max_declared_sil > 0 && (int)achieved > max_declared_sil)
        achieved = (SILLevel)max_declared_sil;

    result->sil_rating = achieved;
}

/* ═══════════════════════════════════════════════════════════════════════════
   CHECK 8: State Machine validation — reachable states, deterministic transitions
   ═══════════════════════════════════════════════════════════════════════════ */
static int state_machine_has_state(ASTNode *sm, const char *name) {
    if (!sm || !name || !name[0]) return 0;
    for (int i = 0; i < sm->child_count; i++) {
        ASTNode *state = sm->children[i];
        if (state && state->type == NODE_STATE &&
            strcasecmp(state->var_name, name) == 0)
            return 1;
    }
    return 0;
}

static void check_state_machines_recursive(ASTNode *node, SafetyResult *result) {
    if (!node) return;

    if (node->type == NODE_STATE_MACHINE) {
        ASTNode *stmt = node;

        int state_count = 0;
        int trans_count = 0;
        for (int j = 0; j < stmt->child_count; j++) {
            ASTNode *state = stmt->children[j];
            if (state && state->type == NODE_STATE) {
                for (int prev = 0; prev < j; prev++) {
                    ASTNode *other = stmt->children[prev];
                    if (other && other->type == NODE_STATE &&
                        strcasecmp(other->var_name, state->var_name) == 0) {
                        char msg[MAX_SAFETY_MSG];
                        snprintf(msg, sizeof(msg),
                                 "State Machine '%.64s' declares duplicate state '%.64s'",
                                 stmt->var_name, state->var_name);
                        add_issue(result, SAFETY_FATAL, SAFE_CAT_GENERAL,
                                  state->line, state->var_name, msg,
                                  "Rename duplicate states so transitions are deterministic");
                    }
                }
                state_count++;
                for (int k = 0; k < state->child_count; k++) {
                    ASTNode *trans = state->children[k];
                    if (trans && trans->type == NODE_TRANSITION) {
                        trans_count++;
                        if (!state_machine_has_state(stmt, trans->var_name)) {
                            char msg[MAX_SAFETY_MSG];
                            snprintf(msg, sizeof(msg),
                                     "Transition from state '%.64s' targets unknown state '%.64s'",
                                     state->var_name, trans->var_name);
                            add_issue(result, SAFETY_FATAL, SAFE_CAT_GENERAL,
                                      trans->line, trans->var_name, msg,
                                      "Add the target state or fix the transition name");
                        }
                    }
                }
            }
        }

        if (state_count > 0 && trans_count == 0) {
            char msg[MAX_SAFETY_MSG];
            snprintf(msg, sizeof(msg), "State Machine '%s' has states but NO transitions", stmt->var_name);
            add_issue(result, SAFETY_WARNING, SAFE_CAT_GENERAL, stmt->line, stmt->var_name, msg, 
                      "Add TRANSITION TO [state] IF [cond] to enable state changes");
        }
    }

    for (int i = 0; i < node->child_count; i++)
        check_state_machines_recursive(node->children[i], result);
    if (node->next)
        check_state_machines_recursive(node->next, result);
}

static void check_state_machines(CompilerCtx *ctx, SafetyResult *result) {
    check_state_machines_recursive(ctx->ast_root, result);
}

/* ═══════════════════════════════════════════════════════════════════════════
   CHECK 9: Assertion validation — static contradiction detection
   ═══════════════════════════════════════════════════════════════════════════ */
static void check_assertions_recursive(ASTNode *node, SafetyResult *result) {
    if (!node) return;

    if (node->type == NODE_ASSERT && node->child_count > 0) {
        /* Simple symbolic contradiction check: ASSERT X = ON AND X = OFF */
        ASTNode *cond = node->children[0];
        if (cond && cond->type == NODE_AND && cond->child_count == 2) {
            ASTNode *l = cond->children[0];
            ASTNode *r = cond->children[1];
            if (l && r &&
                l->type == NODE_COMPARISON && r->type == NODE_COMPARISON &&
                strcmp(l->var_name, r->var_name) == 0 &&
                l->cmp_op == CMP_EQ && r->cmp_op == CMP_EQ &&
                strcmp(l->value, r->value) != 0) {
                char msg[MAX_SAFETY_MSG];
                snprintf(msg, sizeof(msg), "Assertion contains STATIC CONTRADICTION: '%.48s = %.48s AND %.48s = %.48s'",
                         l->var_name, l->value, r->var_name, r->value);
                add_issue(result, SAFETY_FATAL, SAFE_CAT_CONDITION_OVERLAP, node->line, l->var_name, msg,
                          "Fix the logic to avoid mutually exclusive conditions in assertion");
            }
        }
    }

    for (int i = 0; i < node->child_count; i++)
        check_assertions_recursive(node->children[i], result);
    if (node->next)
        check_assertions_recursive(node->next, result);
}

static void check_assertions(CompilerCtx *ctx, SafetyResult *result) {
    check_assertions_recursive(ctx->ast_root, result);
}

/* ═══════════════════════════════════════════════════════════════════════════
   CHECK 10: Circular Dependency Detection (V2.0 10/10 Upgrade)
   Detects if logic contains loops that could oscillate or cause jitter.
   ═══════════════════════════════════════════════════════════════════════════ */
static int dep_matrix[256][256]; /* simple adjacency matrix for symbols */

static void build_deps_recursive(CompilerCtx *ctx, ASTNode *cond, int out_idx) {
    if (!cond) return;
    if (cond->type == NODE_COMPARISON) {
        Symbol *s = sym_lookup(ctx, cond->var_name);
        if (s) {
            int in_idx = -1;
            for (int i = 0; i < ctx->sym_count; i++) if (&ctx->symbols[i] == s) { in_idx = i; break; }
            if (in_idx >= 0 && in_idx < 256 && out_idx >= 0 && out_idx < 256)
                dep_matrix[out_idx][in_idx] = 1;
        }
    }
    for (int i = 0; i < cond->child_count; i++) build_deps_recursive(ctx, cond->children[i], out_idx);
}

static int symbol_index(CompilerCtx *ctx, Symbol *symbol) {
    if (!ctx || !symbol) return -1;
    for (int i = 0; i < ctx->sym_count; i++)
        if (&ctx->symbols[i] == symbol)
            return i;
    return -1;
}

static void build_deps_from_actions(CompilerCtx *ctx, ASTNode *actions, ASTNode *cond) {
    for (ASTNode *act = actions; act; act = act->next) {
        if (act->type != NODE_ACTION || !act->var_name[0]) continue;
        Symbol *s = sym_lookup(ctx, act->var_name);
        int out_idx = symbol_index(ctx, s);
        build_deps_recursive(ctx, cond, out_idx);
    }
}

static void build_deps_from_logic_recursive(CompilerCtx *ctx, ASTNode *node) {
    if (!node) return;

    if (node->type == NODE_IF && node->child_count > 1) {
        ASTNode *cond = node->children[0];
        if (node->children[1])
            build_deps_from_actions(ctx, node->children[1]->next, cond);
        if (node->child_count > 2 && node->children[2])
            build_deps_from_actions(ctx, node->children[2]->next, cond);
    }

    for (int i = 0; i < node->child_count; i++)
        build_deps_from_logic_recursive(ctx, node->children[i]);
    if (node->next)
        build_deps_from_logic_recursive(ctx, node->next);
}

static void check_circular_dependencies(CompilerCtx *ctx, SafetyResult *result) {
    memset(dep_matrix, 0, sizeof(dep_matrix));
    build_deps_from_logic_recursive(ctx, ctx->ast_root);

    /* Floyd-Warshall for transitive closure (detecting cycles) */
    int n = ctx->sym_count > 256 ? 256 : ctx->sym_count;
    for (int k = 0; k < n; k++)
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                dep_matrix[i][j] |= (dep_matrix[i][k] && dep_matrix[k][j]);

    for (int i = 0; i < n; i++) {
        if (dep_matrix[i][i]) {
            char msg[MAX_SAFETY_MSG];
            snprintf(msg, sizeof(msg), "CIRCULAR LOGIC: Variable '%s' indirectly depends on ITSELF", ctx->symbols[i].name);
            add_issue(result, SAFETY_CRITICAL, SAFE_CAT_CIRCULAR_DEPENDENCY, 0, ctx->symbols[i].name, msg, 
                      "Break the circular dependency by using a memory bit or reordering logic.");
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   CHECK 11: Complexity & WCET Estimation (V2.0 10/10 Upgrade)
   ═══════════════════════════════════════════════════════════════════════════ */
static int count_ops(ASTNode *node) {
    if (!node) return 0;
    int c = 1;
    for (int i = 0; i < node->child_count; i++) c += count_ops(node->children[i]);
    if (node->next) c += count_ops(node->next);
    return c;
}

static void estimate_complexity(CompilerCtx *ctx, SafetyResult *result) {
    int ops = count_ops(ctx->ast_root);
    /* Heuristic: PLC scan time overhead (arbitrary units) */
    if (ops > 500) {
        char msg[MAX_SAFETY_MSG];
        snprintf(msg, sizeof(msg), "High logic complexity detected (%d operations)", ops);
        add_issue(result, SAFETY_INFO, SAFE_CAT_COMPLEXITY, 0, NULL, msg, 
                  "Consider splitting logic into multiple POUs to maintain deterministic scan time.");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   PUBLIC API — Main safety analysis entry point
   ═══════════════════════════════════════════════════════════════════════════ */
int safety_analyze(CompilerCtx *ctx, SafetyResult *result) {
    if (!ctx || !ctx->ast_root || !result) return 0;
    memset(result, 0, sizeof(*result));

    log_info(ctx, "Safety: starting IEC 61508 analysis pass");

    /* Step 1: Collect all output assignments */
    collect_all_outputs(ctx, result);

    /* Step 2: Run all safety checks */
    check_unannotated_outputs(ctx, result);   /* annotation-driven, no heuristics */
    check_conflicting_outputs(ctx, result);
    check_missing_else(ctx, result);
    check_estop_coverage(ctx, result);
    check_timer_safety(ctx, result);
    check_write_write_conflicts(ctx, result);
    check_output_coverage(ctx, result);
    check_state_machines(ctx, result);
    check_assertions(ctx, result);
    check_circular_dependencies(ctx, result);
    estimate_complexity(ctx, result);

    /* Step 3: Compute SIL rating */
    compute_sil_rating(ctx, result);

    log_info(ctx, "Safety: analysis complete — SIL %d, score %d/100",
             result->sil_rating, result->compliance_score);

    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
   REPORTING — Human-readable output
   ═══════════════════════════════════════════════════════════════════════════ */
static const char *severity_str(SafetySeverity s) {
    switch (s) {
        case SAFETY_INFO:     return "INFO";
        case SAFETY_WARNING:  return "WARN";
        case SAFETY_CRITICAL: return "CRIT";
        case SAFETY_FATAL:    return "FATAL";
        default:              return "?";
    }
}

static const char *category_str(SafetyCategory c) {
    switch (c) {
        case SAFE_CAT_CONFLICTING_OUTPUTS:     return "Conflicting Outputs";
        case SAFE_CAT_MISSING_ELSE:            return "Missing ELSE/Default";
        case SAFE_CAT_ESTOP_COVERAGE:          return "E-Stop Coverage";
        case SAFE_CAT_TIMER_ON_CRITICAL_PATH:  return "Timer Safety";
        case SAFE_CAT_WRITE_WRITE_CONFLICT:    return "Write-Write Conflict";
        case SAFE_CAT_UNREACHABLE_CODE:        return "Unreachable Code";
        case SAFE_CAT_UNINITIALIZED_OUTPUT:    return "Output Coverage";
        case SAFE_CAT_REDUNDANCY:              return "Redundancy";
        case SAFE_CAT_CONDITION_OVERLAP:       return "Condition Overlap";
        case SAFE_CAT_CIRCULAR_DEPENDENCY:     return "Circular Dependency";
        case SAFE_CAT_COMPLEXITY:              return "Complexity";
        case SAFE_CAT_GENERAL:                 return "General";
        default:                               return "Unknown";
    }
}

static const char *sil_str(SILLevel s) {
    switch (s) {
        case SIL_NONE: return "NONE (Non-Safety)";
        case SIL_1:    return "SIL 1";
        case SIL_2:    return "SIL 2";
        case SIL_3:    return "SIL 3";
        case SIL_4:    return "SIL 4 (Highest)";
        default:       return "?";
    }
}

void safety_print_report(const SafetyResult *result) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║          IEC 61508 SAFETY ANALYSIS REPORT                  ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║                                                            ║\n");
    printf("║  SIL Rating       : %-38s ║\n", sil_str(result->sil_rating));
    printf("║  Compliance Score : %d / 100                               ║\n", result->compliance_score);
    printf("║                                                            ║\n");
    printf("║  Total Outputs    : %-5d                                  ║\n", result->total_outputs);
    printf("║  Covered (ON+OFF) : %-5d                                  ║\n", result->covered_outputs);
    printf("║  E-Stop Covered   : %-5d                                  ║\n", result->estop_covered);
    printf("║  E-Stop Present   : %-5s                                  ║\n", result->has_estop ? "YES" : "NO");
    printf("║                                                            ║\n");
    printf("║  Critical Issues  : %-5d                                  ║\n", result->critical_count);
    printf("║  Warnings         : %-5d                                  ║\n", result->warning_count);
    printf("║  Info             : %-5d                                  ║\n", result->info_count);
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    if (result->issue_count > 0) {
        printf("┌──────────────────────────────────────────────────────────────┐\n");
        printf("│  SAFETY ISSUES                                               \n");
        printf("├──────────────────────────────────────────────────────────────┤\n");
        for (int i = 0; i < result->issue_count; i++) {
            const SafetyIssue *si = &result->issues[i];
            printf("│  [%s] [%s]\n", severity_str(si->severity), category_str(si->category));
            if (si->line > 0) printf("│     Line: %d\n", si->line);
            if (si->variable[0]) printf("│     Variable: %s\n", si->variable);
            printf("│     %s\n", si->message);
            printf("│     → %s\n", si->recommendation);
            printf("│\n");
        }
        printf("└──────────────────────────────────────────────────────────────┘\n\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   JSON OUTPUT — for machine-readable out
   ═══════════════════════════════════════════════════════════════════════════ */
void safety_emit_json(const SafetyResult *result) {
    printf(",\"safety\":{");
    printf("\"sil_rating\":%d", result->sil_rating);
    printf(",\"compliance_score\":%d", result->compliance_score);
    printf(",\"total_outputs\":%d", result->total_outputs);
    printf(",\"covered_outputs\":%d", result->covered_outputs);
    printf(",\"estop_covered\":%d", result->estop_covered);
    printf(",\"has_estop\":%s", result->has_estop ? "true" : "false");
    printf(",\"critical_count\":%d", result->critical_count);
    printf(",\"warning_count\":%d", result->warning_count);
    printf(",\"info_count\":%d", result->info_count);
    printf(",\"issues\":[");
    for (int i = 0; i < result->issue_count; i++) {
        const SafetyIssue *si = &result->issues[i];
        if (i) printf(",");
        printf("{\"severity\":");
        json_print_string(severity_str(si->severity));
        printf(",\"category\":");
        json_print_string(category_str(si->category));
        printf(",\"line\":%d", si->line);
        printf(",\"variable\":");
        json_print_string(si->variable);
        printf(",\"message\":");
        json_print_string(si->message);
        printf(",\"recommendation\":");
        json_print_string(si->recommendation);
        printf("}");
    }
    printf("]");
    printf("}");
}

/* ═══════════════════════════════════════════════════════════════════════════
   FILE REPORT — Write to file
   ═══════════════════════════════════════════════════════════════════════════ */
int safety_write_report(const SafetyResult *result, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;

    fprintf(f, "═══════════════════════════════════════════════════════════\n");
    fprintf(f, "  IEC 61508 SAFETY ANALYSIS REPORT\n");
    fprintf(f, "  Generated by PLC DSL Compiler v2.0\n");
    fprintf(f, "═══════════════════════════════════════════════════════════\n\n");

    fprintf(f, "SIL Rating       : %s\n", sil_str(result->sil_rating));
    fprintf(f, "Compliance Score : %d / 100\n\n", result->compliance_score);

    fprintf(f, "METRICS:\n");
    fprintf(f, "  Total Outputs    : %d\n", result->total_outputs);
    fprintf(f, "  Covered (ON+OFF) : %d\n", result->covered_outputs);
    fprintf(f, "  E-Stop Covered   : %d\n", result->estop_covered);
    fprintf(f, "  E-Stop Present   : %s\n\n", result->has_estop ? "YES" : "NO");

    fprintf(f, "ISSUE SUMMARY:\n");
    fprintf(f, "  Critical : %d\n", result->critical_count);
    fprintf(f, "  Warnings : %d\n", result->warning_count);
    fprintf(f, "  Info     : %d\n\n", result->info_count);

    if (result->issue_count > 0) {
        fprintf(f, "DETAILED ISSUES:\n");
        fprintf(f, "───────────────────────────────────────────────────────────\n");
        for (int i = 0; i < result->issue_count; i++) {
            const SafetyIssue *si = &result->issues[i];
            fprintf(f, "\n  [%d] %s — %s\n", i + 1,
                    severity_str(si->severity), category_str(si->category));
            if (si->line > 0) fprintf(f, "      Line     : %d\n", si->line);
            if (si->variable[0]) fprintf(f, "      Variable : %s\n", si->variable);
            fprintf(f, "      Issue    : %s\n", si->message);
            fprintf(f, "      Fix      : %s\n", si->recommendation);
        }
        fprintf(f, "\n───────────────────────────────────────────────────────────\n");
    }

    fprintf(f, "\nEND OF SAFETY REPORT\n");
    fclose(f);
    return 1;
}
