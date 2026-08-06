#include "plc_compiler.h"

typedef struct {
    char *buf;
    int len;
    int cap;
} TranslateBuf;

typedef enum {
    ADDR_UNKNOWN = 0,
    ADDR_INPUT,
    ADDR_OUTPUT,
    ADDR_MEMORY,
    ADDR_TIMER
} AddressClass;

static void tb_init(TranslateBuf *tb) {
    tb->cap = 4096;
    tb->len = 0;
    tb->buf = (char *)malloc((size_t)tb->cap);
    if (tb->buf) tb->buf[0] = '\0';
}

static void tb_free(TranslateBuf *tb) {
    free(tb->buf);
    tb->buf = NULL;
}

static void tb_append(TranslateBuf *tb, const char *fmt, ...) {
    if (!tb->buf) return;

    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) return;

    if (tb->len + needed + 1 >= tb->cap) {
        int new_cap = tb->cap;
        while (tb->len + needed + 1 >= new_cap) new_cap *= 2;
        char *new_buf = (char *)realloc(tb->buf, (size_t)new_cap);
        if (!new_buf) return;
        tb->buf = new_buf;
        tb->cap = new_cap;
    }

    va_start(args, fmt);
    tb->len += vsnprintf(tb->buf + tb->len, (size_t)(tb->cap - tb->len), fmt, args);
    va_end(args);
}

static const char *target_cli_name(PLCTarget t) {
    switch (t) {
        case PLC_SIEMENS_TIA: return "siemens";
        case PLC_CODESYS: return "codesys";
        case PLC_ROCKWELL_AOI: return "rockwell";
        default: return "unknown";
    }
}

static const char *target_display_name(PLCTarget t) {
    switch (t) {
        case PLC_SIEMENS_TIA: return "Siemens TIA Portal (S7-1200/1500)";
        case PLC_CODESYS: return "CODESYS V3 (IEC 61131-3)";
        case PLC_ROCKWELL_AOI: return "Rockwell Studio 5000 (AOI)";
        default: return "Unknown";
    }
}

static int starts_with_trimmed(const char *line, const char *prefix) {
    while (*line == ' ' || *line == '\t') line++;
    return strncmp(line, prefix, strlen(prefix)) == 0;
}

static void trim_copy(const char *src, char *dst, int dstlen) {
    while (*src == ' ' || *src == '\t') src++;
    int len = (int)strlen(src);
    while (len > 0 && (src[len - 1] == '\r' || src[len - 1] == '\n' || src[len - 1] == ' ' || src[len - 1] == '\t')) len--;
    if (len >= dstlen) len = dstlen - 1;
    memcpy(dst, src, (size_t)len);
    dst[len] = '\0';
}

static void replace_all(char *line, int cap, const char *from, const char *to) {
    char tmp[2048];
    char original[2048];
    char *cursor;
    char *pos;

    strncpy(original, line, sizeof(original) - 1);
    original[sizeof(original) - 1] = '\0';
    cursor = original;
    pos = strstr(cursor, from);
    if (!pos) return;

    tmp[0] = '\0';
    while (pos) {
        strncat(tmp, cursor, (size_t)(pos - cursor));
        strncat(tmp, to, sizeof(tmp) - strlen(tmp) - 1);
        cursor = pos + strlen(from);
        pos = strstr(cursor, from);
    }
    strncat(tmp, cursor, sizeof(tmp) - strlen(tmp) - 1);
    strncpy(line, tmp, (size_t)cap - 1);
    line[cap - 1] = '\0';
}

static int parse_address(const char *addr, AddressClass *klass, int *major, int *minor) {
    if (sscanf(addr, "%%I%d.%d", major, minor) == 2) { *klass = ADDR_INPUT; return 1; }
    if (sscanf(addr, "%%Q%d.%d", major, minor) == 2) { *klass = ADDR_OUTPUT; return 1; }
    if (sscanf(addr, "%%M%d.%d", major, minor) == 2) { *klass = ADDR_MEMORY; return 1; }
    if (sscanf(addr, "%%IX%d.%d", major, minor) == 2) { *klass = ADDR_INPUT; return 1; }
    if (sscanf(addr, "%%QX%d.%d", major, minor) == 2) { *klass = ADDR_OUTPUT; return 1; }
    if (sscanf(addr, "%%MX%d.%d", major, minor) == 2) { *klass = ADDR_MEMORY; return 1; }
    if (sscanf(addr, "I:%d/%d", major, minor) == 2) { *klass = ADDR_INPUT; return 1; }
    if (sscanf(addr, "O:%d/%d", major, minor) == 2) { *klass = ADDR_OUTPUT; return 1; }
    if (sscanf(addr, "B3:%d/%d", major, minor) == 2) { *klass = ADDR_MEMORY; return 1; }
    if (sscanf(addr, "T%d", major) == 1) { *klass = ADDR_TIMER; *minor = 0; return 1; }
    if (sscanf(addr, "TIMER_%d", major) == 1) { *klass = ADDR_TIMER; *minor = 0; return 1; }
    if (sscanf(addr, "T4:%d", major) == 1) { *klass = ADDR_TIMER; *minor = 0; return 1; }
    return 0;
}

static void format_address(AddressClass klass, int major, int minor, PLCTarget target,
                           char *out, int outlen) {
    switch (klass) {
        case ADDR_INPUT:
            if (target == PLC_SIEMENS_TIA) snprintf(out, outlen, "%%I%d.%d", major, minor);
            else if (target == PLC_CODESYS) snprintf(out, outlen, "%%IX%d.%d", major, minor);
            else snprintf(out, outlen, "I:%d/%d", major, minor);
            break;
        case ADDR_OUTPUT:
            if (target == PLC_SIEMENS_TIA) snprintf(out, outlen, "%%Q%d.%d", major, minor);
            else if (target == PLC_CODESYS) snprintf(out, outlen, "%%QX%d.%d", major, minor);
            else snprintf(out, outlen, "O:%d/%d", major, minor);
            break;
        case ADDR_MEMORY:
            if (target == PLC_SIEMENS_TIA) snprintf(out, outlen, "%%M%d.%d", major, minor);
            else if (target == PLC_CODESYS) snprintf(out, outlen, "%%MX%d.%d", major, minor);
            else snprintf(out, outlen, "B3:%d/%d", major, minor);
            break;
        case ADDR_TIMER:
            if (target == PLC_SIEMENS_TIA) snprintf(out, outlen, "T%d", major);
            else if (target == PLC_CODESYS) snprintf(out, outlen, "TIMER_%d", major);
            else snprintf(out, outlen, "T4:%d", major);
            break;
        default:
            snprintf(out, outlen, "%d", major);
            break;
    }
}

static int parse_variable_line(const char *line, char *name, int name_len,
                               char *addr, int addr_len, char *type, int type_len) {
    const char *at = strstr(line, " AT ");
    const char *colon = strstr(line, " : ");
    const char *semi = strstr(line, ";");
    if (!at || !colon || !semi || at > colon) return 0;

    const char *name_start = line;
    while (*name_start == ' ' || *name_start == '\t') name_start++;
    if (strncmp(name_start, "(*", 2) == 0) {
        const char *after_comment = strstr(name_start, "*)");
        if (!after_comment) return 0;
        name_start = after_comment + 2;
        while (*name_start == ' ' || *name_start == '\t') name_start++;
    }

    int name_sz = (int)(at - name_start);
    while (name_sz > 0 && (name_start[name_sz - 1] == ' ' || name_start[name_sz - 1] == '\t')) name_sz--;
    if (name_sz <= 0 || name_sz >= name_len) return 0;
    memcpy(name, name_start, (size_t)name_sz);
    name[name_sz] = '\0';

    const char *addr_start = at + 4;
    int addr_sz = (int)(colon - addr_start);
    while (addr_sz > 0 && (addr_start[addr_sz - 1] == ' ' || addr_start[addr_sz - 1] == '\t')) addr_sz--;
    if (addr_sz <= 0 || addr_sz >= addr_len) return 0;
    memcpy(addr, addr_start, (size_t)addr_sz);
    addr[addr_sz] = '\0';

    const char *type_start = colon + 3;
    int type_sz = (int)(semi - type_start);
    while (type_sz > 0 && (type_start[type_sz - 1] == ' ' || type_start[type_sz - 1] == '\t')) type_sz--;
    if (type_sz <= 0 || type_sz >= type_len) return 0;
    memcpy(type, type_start, (size_t)type_sz);
    type[type_sz] = '\0';
    return 1;
}

static int parse_timer_decl(const char *line, char *name, int name_len, int *timer_idx) {
    (void)name_len;
    if (!starts_with_trimmed(line, "_TIMER_")) return 0;
    if (sscanf(line, " %127[^ :]", name) != 1) return 0;
    if (sscanf(name, "_TIMER_%d", timer_idx) != 1) *timer_idx = 0;
    return 1;
}

static int parse_timer_call_normalized(const char *line, char *timer_name, int timer_len,
                                       char *expr, int expr_len, char *pt, int pt_len) {
    const char *lp = strchr(line, '(');
    const char *in = strstr(line, "IN :=");
    const char *pt_marker = strstr(line, ", PT :=");
    const char *rp = strrchr(line, ')');
    if (!lp || !in || !pt_marker || !rp || lp > in || in > pt_marker) return 0;

    int name_sz = (int)(lp - line);
    while (name_sz > 0 && (line[name_sz - 1] == ' ' || line[name_sz - 1] == '\t')) name_sz--;
    while (*line == ' ' || *line == '\t') { line++; name_sz--; }
    if (name_sz <= 0 || name_sz >= timer_len) return 0;
    memcpy(timer_name, line, (size_t)name_sz);
    timer_name[name_sz] = '\0';

    const char *expr_start = in + 5;
    while (*expr_start == ' ' || *expr_start == '\t') expr_start++;
    int expr_sz = (int)(pt_marker - expr_start);
    while (expr_sz > 0 && (expr_start[expr_sz - 1] == ' ' || expr_start[expr_sz - 1] == '\t')) expr_sz--;
    if (expr_sz <= 0 || expr_sz >= expr_len) return 0;
    memcpy(expr, expr_start, (size_t)expr_sz);
    expr[expr_sz] = '\0';

    const char *pt_start = pt_marker + 7;
    while (*pt_start == ' ' || *pt_start == '\t') pt_start++;
    int pt_sz = (int)(rp - pt_start);
    while (pt_sz > 0 && (pt_start[pt_sz - 1] == ' ' || pt_start[pt_sz - 1] == '\t' || pt_start[pt_sz - 1] == ';')) pt_sz--;
    if (pt_sz <= 0 || pt_sz >= pt_len) return 0;
    memcpy(pt, pt_start, (size_t)pt_sz);
    pt[pt_sz] = '\0';
    return 1;
}

static int parse_timer_call_rockwell(const char *line, char *timer_name, int timer_len,
                                     char *expr, int expr_len, char *pt, int pt_len) {
    char trimmed[512];
    trim_copy(line, trimmed, sizeof(trimmed));
    if (strncmp(trimmed, "TON(", 4) != 0) return 0;

    const char *cursor = trimmed + 4;
    const char *comma1 = strchr(cursor, ',');
    const char *comma2 = comma1 ? strchr(comma1 + 1, ',') : NULL;
    const char *rp = strrchr(trimmed, ')');
    if (!comma1 || !comma2 || !rp) return 0;

    int name_sz = (int)(comma1 - cursor);
    while (name_sz > 0 && cursor[name_sz - 1] == ' ') name_sz--;
    if (name_sz <= 0 || name_sz >= timer_len) return 0;
    memcpy(timer_name, cursor, (size_t)name_sz);
    timer_name[name_sz] = '\0';

    const char *expr_start = comma1 + 1;
    while (*expr_start == ' ') expr_start++;
    int expr_sz = (int)(comma2 - expr_start);
    while (expr_sz > 0 && expr_start[expr_sz - 1] == ' ') expr_sz--;
    if (expr_sz <= 0 || expr_sz >= expr_len) return 0;
    memcpy(expr, expr_start, (size_t)expr_sz);
    expr[expr_sz] = '\0';

    const char *pt_start = comma2 + 1;
    while (*pt_start == ' ') pt_start++;
    int pt_sz = (int)(rp - pt_start);
    while (pt_sz > 0 && (pt_start[pt_sz - 1] == ' ' || pt_start[pt_sz - 1] == ';')) pt_sz--;
    if (pt_sz <= 0 || pt_sz >= pt_len) return 0;
    memcpy(pt, pt_start, (size_t)pt_sz);
    pt[pt_sz] = '\0';
    return 1;
}

static int is_wrapper_line(const char *trimmed) {
    return strcmp(trimmed, "ORGANIZATION_BLOCK OB1") == 0 ||
           strcmp(trimmed, "PROGRAM PLC_PRG") == 0 ||
           strcmp(trimmed, "(* Add-On Instruction (AOI) Block *)") == 0 ||
           strcmp(trimmed, "ROUTINE MainRoutine") == 0 ||
           strcmp(trimmed, "END_ORGANIZATION_BLOCK") == 0 ||
           strcmp(trimmed, "END_PROGRAM") == 0 ||
           strcmp(trimmed, "END_ROUTINE") == 0 ||
           strncmp(trimmed, "TITLE =", 7) == 0 ||
           strncmp(trimmed, "VERSION :", 9) == 0;
}

static void emit_wrapper(TranslateBuf *tb, PLCTarget target) {
    if (target == PLC_SIEMENS_TIA) {
        tb_append(tb, "ORGANIZATION_BLOCK OB1\n");
        tb_append(tb, "TITLE = 'Main Program'\n");
        tb_append(tb, "VERSION : '0.1'\n\n");
    } else if (target == PLC_CODESYS) {
        tb_append(tb, "PROGRAM PLC_PRG\n\n");
    } else {
        tb_append(tb, "(* Add-On Instruction (AOI) Block *)\n");
        tb_append(tb, "ROUTINE MainRoutine\n\n");
    }
}

static int translate_line(const char *line, TranslateBuf *tb, PLCTarget from, PLCTarget to,
                          int *wrapper_emitted, const char *out_path) {
    char trimmed[1024];
    trim_copy(line, trimmed, sizeof(trimmed));

    if (trimmed[0] == '\0') {
        tb_append(tb, "\n");
        return 1;
    }

    if (strcmp(trimmed, "(* ============================================================= *)") == 0 ||
        strncmp(trimmed, "(*  AUTO-GENERATED BY PLC DSL COMPILER", 38) == 0 ||
        strncmp(trimmed, "(*  Format  :", 13) == 0 ||
        strncmp(trimmed, "(*  Date    :", 13) == 0 ||
        strncmp(trimmed, "(*  WARNING :", 13) == 0 ||
        strncmp(trimmed, "(* Compiled:", 12) == 0) {
        return 1;
    }

    if (strncmp(trimmed, "(*  Target  :", 13) == 0) {
        tb_append(tb, "(*  Target  : %-48s*)\n", target_display_name(to));
        return 1;
    }
    if (strncmp(trimmed, "(*  Source  :", 13) == 0) {
        tb_append(tb, "(*  Source  : %-48s*)\n", out_path);
        return 1;
    }

    if (is_wrapper_line(trimmed)) {
        if (!*wrapper_emitted && strchr(trimmed, 'B') != NULL) {
            emit_wrapper(tb, to);
            *wrapper_emitted = 1;
        }
        return 1;
    }

    if (strcmp(trimmed, "(* === Variable Declarations === *)") == 0 && !*wrapper_emitted) {
        emit_wrapper(tb, to);
        *wrapper_emitted = 1;
    }

    {
        char timer_name[128], expr[256], pt[64];
        if (parse_timer_call_rockwell(trimmed, timer_name, sizeof(timer_name), expr, sizeof(expr), pt, sizeof(pt))) {
            if (to == PLC_ROCKWELL_AOI) tb_append(tb, "%s\n", trimmed);
            else tb_append(tb, "%s(IN := %s, PT := %s);\n", timer_name, expr, pt);
            return 1;
        }
        if (parse_timer_call_normalized(trimmed, timer_name, sizeof(timer_name), expr, sizeof(expr), pt, sizeof(pt))) {
            if (to == PLC_ROCKWELL_AOI) tb_append(tb, "TON(%s, %s, %s);\n", timer_name, expr, pt);
            else tb_append(tb, "%s(IN := %s, PT := %s);\n", timer_name, expr, pt);
            return 1;
        }
    }

    {
        char timer_name[128];
        int timer_idx = 0;
        if (parse_timer_decl(trimmed, timer_name, sizeof(timer_name), &timer_idx)) {
            if (to == PLC_SIEMENS_TIA) tb_append(tb, "    %s AT T%d : TON;  (* Timer On-Delay #%d *)\n", timer_name, timer_idx, timer_idx);
            else if (to == PLC_CODESYS) tb_append(tb, "    %s : TON;  (* Timer #%d *)\n", timer_name, timer_idx);
            else tb_append(tb, "    %s : TIMER;  (* T4:%d *)\n", timer_name, timer_idx);
            return 1;
        }
    }

    {
        char name[128], addr[128], type[64], new_addr[128];
        AddressClass klass;
        int major = 0, minor = 0;
        if (parse_variable_line(trimmed, name, sizeof(name), addr, sizeof(addr), type, sizeof(type)) &&
            parse_address(addr, &klass, &major, &minor)) {
            if (strncmp(name, "_TIMER_", 7) == 0) {
                int timer_idx = 0;
                if (sscanf(name, "_TIMER_%d", &timer_idx) != 1) timer_idx = major;
                if (to == PLC_SIEMENS_TIA) tb_append(tb, "    %s AT T%d : TON;  (* Timer On-Delay #%d *)\n", name, timer_idx, timer_idx);
                else if (to == PLC_CODESYS) tb_append(tb, "    %s : TON;  (* Timer #%d *)\n", name, timer_idx);
                else tb_append(tb, "    %s : TIMER;  (* T4:%d *)\n", name, timer_idx);
                return 1;
            }
            format_address(klass, major, minor, to, new_addr, sizeof(new_addr));
            tb_append(tb, "    (* VAR_%s *) %s AT %s : %s;  (* Physical %s *)\n",
                      klass == ADDR_INPUT ? "INPUT" : (klass == ADDR_OUTPUT ? "OUTPUT" : "MEMORY"),
                      name, new_addr, type,
                      klass == ADDR_INPUT ? "Input" : (klass == ADDR_OUTPUT ? "Output" : "Memory"));
            return 1;
        }
    }

    {
        char converted[2048];
        strncpy(converted, trimmed, sizeof(converted) - 1);
        converted[sizeof(converted) - 1] = '\0';
        if (to == PLC_ROCKWELL_AOI) {
            replace_all(converted, sizeof(converted), ".Q", ".DN");
        } else {
            replace_all(converted, sizeof(converted), ".DN", ".Q");
        }

        replace_all(converted, sizeof(converted), "Siemens TIA Portal (S7-1200/1500)", target_display_name(to));
        replace_all(converted, sizeof(converted), "CODESYS V3 (IEC 61131-3)", target_display_name(to));
        replace_all(converted, sizeof(converted), "Rockwell Studio 5000 (AOI)", target_display_name(to));
        tb_append(tb, "%s\n", converted);
    }

    (void)from;
    return 1;
}

int translate_vendor_code(const char *src_path, const char *out_path,
                          PLCTarget from, PLCTarget to, int quiet_mode) {
    char *source = file_read(src_path);
    if (!source) {
        fprintf(stderr, "Failed to read source file '%s'\n", src_path);
        return 0;
    }

    TranslateBuf tb;
    tb_init(&tb);
    if (!tb.buf) {
        free(source);
        return 0;
    }

    tb_append(&tb, "(* ============================================================= *)\n");
    tb_append(&tb, "(*  AUTO-GENERATED BY PLC DSL COMPILER v2.0                    *)\n");
    tb_append(&tb, "(*  Translation: %s -> %s *)\n",
              target_cli_name(from), target_cli_name(to));

    int wrapper_emitted = 0;
    char *cursor = source;
    while (*cursor) {
        char *line_end = strchr(cursor, '\n');
        int len = line_end ? (int)(line_end - cursor) : (int)strlen(cursor);
        char line[2048];
        if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;
        memcpy(line, cursor, (size_t)len);
        line[len] = '\0';

        translate_line(line, &tb, from, to, &wrapper_emitted, out_path);
        cursor = line_end ? line_end + 1 : cursor + len;
    }

    if (!file_write(out_path, tb.buf)) {
        fprintf(stderr, "Failed to write translated output '%s'\n", out_path);
        tb_free(&tb);
        free(source);
        return 0;
    }

    if (!quiet_mode) {
        printf("Translated vendor code: %s -> %s\n", target_cli_name(from), target_cli_name(to));
        printf("Source : %s\n", src_path);
        printf("Output : %s\n", out_path);
    }

    tb_free(&tb);
    free(source);
    return 1;
}
