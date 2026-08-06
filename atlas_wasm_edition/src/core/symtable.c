/**
 * symtable.c — Symbol Table
 * Maps English variable names → PLC addresses for multiple platforms.
 * Auto-classifies I/O direction based on usage context.
 */

#include "plc_compiler.h"

static int ensure_symbol_cap(CompilerCtx *ctx, int extra) {
    if (!ctx) return 0;
    int needed = ctx->sym_count + extra;
    if (ctx->max_symbols > 0 && needed > ctx->max_symbols) {
        log_error(ctx, 0, "Symbol limit exceeded (%d > %d)",
                  needed, ctx->max_symbols);
        return 0;
    }
    if (needed <= ctx->sym_cap) return 1;

    int new_cap = ctx->sym_cap > 0 ? ctx->sym_cap : 1024;
    while (new_cap < needed) {
        if (new_cap > 1000000000) return 0;
        new_cap *= 2;
    }

    Symbol *new_syms = (Symbol *)realloc(ctx->symbols, (size_t)new_cap * sizeof(Symbol));
    if (!new_syms) return 0;
    ctx->symbols = new_syms;
    ctx->sym_cap = new_cap;
    return 1;
}

static void sym_copy_text(char dst[MAX_IDENTIFIER_LEN], const char *src) {
    snprintf(dst, MAX_IDENTIFIER_LEN, "%s", src ? src : "");
}

/* ─── Hash Function (DJB2) ───────────────────────────────────────────────── */
static unsigned int sym_hash(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        /* case-insensitive hash */
        hash = ((hash << 5) + hash) + tolower(c);
    }
    return hash % SYM_HASH_SIZE;
}

/* ─── Look up symbol by name and scope (case-insensitive) ────────────────── */
Symbol *sym_lookup_scoped(CompilerCtx *ctx, const char *name, int scope_id) {
    if (!ctx) return NULL;
    unsigned int h = sym_hash(name);
    
    /* Check bucket */
    for (Symbol *s = ctx->sym_hash[h]; s; s = s->next_in_hash) {
        if (strcasecmp(s->name, name) == 0 && s->scope_id == scope_id)
            return s;
    }
    
    /* Fallback to global scope (0) if not in current scope */
    if (scope_id != 0) {
        for (Symbol *s = ctx->sym_hash[h]; s; s = s->next_in_hash) {
            if (strcasecmp(s->name, name) == 0 && s->scope_id == 0)
                return s;
        }
    }
    return NULL;
}

Symbol *sym_lookup(CompilerCtx *ctx, const char *name) {
    return sym_lookup_scoped(ctx, name, ctx->cur_scope);
}

/* ─── Create sanitized PLC-safe ST name ─────────────────────────────────── */
static void make_st_name(const char *src, char *dst, int dstlen) {
    int j = 0;
    for (int i = 0; src[i] && j < dstlen - 1; i++) {
        char c = src[i];
        if (isalnum((unsigned char)c) || c == '_') dst[j++] = (char)toupper((unsigned char)c);
        else dst[j++] = '_';
    }
    dst[j] = '\0';
    if (!dst[0]) {
        snprintf(dst, dstlen, "V");
    }
    /* Must not start with digit */
    if (isdigit((unsigned char)dst[0])) {
        /* prepend V_ */
        if (dstlen > 3) {
            size_t len = strlen(dst);
            if (len > (size_t)dstlen - 3) len = (size_t)dstlen - 3;
            memmove(dst + 2, dst, len);
            dst[0] = 'V'; dst[1] = '_';
            dst[len + 2] = '\0';
        }
    }
}

/* ─── Insert new symbol ─────────────────────────────────────────────────── */
Symbol *sym_insert_scoped(CompilerCtx *ctx, const char *name, IODirection dir, SymbolKind kind, int scope_id, const char *parent_type, int line, int col) {
    /* Check if already exists in this scope to prevent duplicates */
    Symbol *existing = sym_lookup_scoped(ctx, name, scope_id);
    if (existing) return existing;

    if (!ensure_symbol_cap(ctx, 1)) {
        log_error(ctx, 0, "Out of memory growing symbol table");
        return NULL;
    }
    
    Symbol *s = &ctx->symbols[ctx->sym_count++];
    memset(s, 0, sizeof(*s));
    sym_copy_text(s->name, name);
    s->direction   = dir;
    s->kind        = kind;
    s->is_used     = 0;
    s->timer_index = -1;
    s->scope_id    = scope_id;
    s->line        = line;
    s->col         = col;
    if (parent_type) {
        sym_copy_text(s->parent_type, parent_type);
    }

    make_st_name(name, s->st_name, MAX_IDENTIFIER_LEN);
    s->plc_address[0] = '\0'; /* assigned later */
    
    /* Add to hash table */
    unsigned int h = sym_hash(name);
    s->next_in_hash = ctx->sym_hash[h];
    ctx->sym_hash[h] = s;

    return s;
}

Symbol *sym_insert(CompilerCtx *ctx, const char *name, IODirection dir, SymbolKind kind, int line, int col) {
    return sym_insert_scoped(ctx, name, dir, kind, ctx->cur_scope, NULL, line, col);
}

/* ─── Auto-assign PLC addresses ─────────────────────────────────────────── */
void sym_assign_addresses(CompilerCtx *ctx) {
    /* Platform-specific address formats:
       Siemens TIA:  %I0.0 (inputs), %Q0.0 (outputs), %M0.0 (memory)
                     IEC Timers: TON, TOF
       CODESYS:      %IX0.0, %QX0.0, %MX0.0
       Rockwell AOI: I:0/0, O:0/0, B3:0/0                    */

    int in_byte = 0, in_bit = 0;
    int out_byte = 0, out_bit = 0;
    int mem_byte = 0, mem_bit = 0;
    int timer_idx = 0;

    for (int i = 0; i < ctx->sym_count; i++) {
        Symbol *s = &ctx->symbols[i];
        char addr[MAX_IDENTIFIER_LEN];

        if (s->kind == SYM_TIMER) {
            s->timer_index = timer_idx++;
            switch (ctx->target) {
                case PLC_SIEMENS_TIA:
                    snprintf(addr, sizeof(addr), "T%d", s->timer_index);
                    break;
                case PLC_CODESYS:
                    snprintf(addr, sizeof(addr), "TIMER_%d", s->timer_index);
                    break;
                case PLC_ROCKWELL_AOI:
                    snprintf(addr, sizeof(addr), "T4:%d", s->timer_index);
                    break;
            }
            sym_copy_text(s->plc_address, addr);
            continue;
        }

        if (s->kind == SYM_CONST) {
            s->plc_address[0] = '\0';
            continue;
        }

        switch (s->direction) {
            case IO_INPUT:
                switch (ctx->target) {
                    case PLC_SIEMENS_TIA:
                        snprintf(addr, sizeof(addr), "%%I%d.%d", in_byte, in_bit); break;
                    case PLC_CODESYS:
                        snprintf(addr, sizeof(addr), "%%IX%d.%d", in_byte, in_bit); break;
                    case PLC_ROCKWELL_AOI:
                        snprintf(addr, sizeof(addr), "I:%d/%d", in_byte, in_bit); break;
                }
                in_bit++;
                if (in_bit >= 8) { in_bit = 0; in_byte++; }
                break;

            case IO_OUTPUT:
                switch (ctx->target) {
                    case PLC_SIEMENS_TIA:
                        snprintf(addr, sizeof(addr), "%%Q%d.%d", out_byte, out_bit); break;
                    case PLC_CODESYS:
                        snprintf(addr, sizeof(addr), "%%QX%d.%d", out_byte, out_bit); break;
                    case PLC_ROCKWELL_AOI:
                        snprintf(addr, sizeof(addr), "O:%d/%d", out_byte, out_bit); break;
                }
                out_bit++;
                if (out_bit >= 8) { out_bit = 0; out_byte++; }
                break;

            case IO_MEMORY:
            default:
                switch (ctx->target) {
                    case PLC_SIEMENS_TIA:
                        snprintf(addr, sizeof(addr), "%%M%d.%d", mem_byte, mem_bit); break;
                    case PLC_CODESYS:
                        snprintf(addr, sizeof(addr), "%%MX%d.%d", mem_byte, mem_bit); break;
                    case PLC_ROCKWELL_AOI:
                        snprintf(addr, sizeof(addr), "B3:%d/%d", mem_byte, mem_bit); break;
                }
                mem_bit++;
                if (mem_bit >= 8) { mem_bit = 0; mem_byte++; }
                break;
        }
        sym_copy_text(s->plc_address, addr);
    }
    log_info(ctx, "Symbol table: assigned addresses to %d symbols", ctx->sym_count);
}

/* ─── Dump symbol table ─────────────────────────────────────────────────── */
void sym_dump(CompilerCtx *ctx) {
    const char *dir_str[] = { "INPUT ", "OUTPUT", "MEMORY", "TIMER ", "LOCAL " };
    const char *knd_str[] = {
        "BOOL ", "INT  ", "REAL ", "TIMER", "STRUCT", "FB   ", "INST ", "CONST"
    };

    printf("\n┌──────────────────────────────────────────────────────────────────┐\n");
    printf("│  SYMBOL TABLE  (%d symbols)                                       \n", ctx->sym_count);
    printf("├──────────────────┬──────────────────┬────────┬────────┬──────────┤\n");
    printf("│ Name             │ ST Name          │ Dir    │ Kind   │ Address  │\n");
    printf("├──────────────────┼──────────────────┼────────┼────────┼──────────┤\n");
    for (int i = 0; i < ctx->sym_count; i++) {
        Symbol *s = &ctx->symbols[i];
        printf("│ %-16s │ %-16s │ %s │ %s │ %-8s │\n",
               s->name, s->st_name,
               s->direction >= 0 && s->direction <= IO_VAR_LOCAL ? dir_str[s->direction] : "UNK   ",
               s->kind >= 0 && s->kind <= SYM_CONST ? knd_str[s->kind] : "UNK  ",
               s->plc_address);
    }
    printf("└──────────────────┴──────────────────┴────────┴────────┴──────────┘\n\n");
}
