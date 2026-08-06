/**
 * export_plcopen.c — PLCopen XML Export (IEC 61131-10)
 *
 * Generates PLCopen TC6 XML interchange format, importable by:
 *   - Siemens TIA Portal
 *   - CODESYS V3
 *   - Beckhoff TwinCAT
 *   - B&R Automation Studio
 *   - ABB Automation Builder
 */

#include "plc_compiler.h"

/* Output buffer reuse (same pattern as codegen.c) */
typedef struct {
  char *buf;
  int len;
  int cap;
} XBuf;

static void xb_init(XBuf *x) {
  x->cap = 1024 * 64;
  x->buf = (char *)malloc(x->cap);
  x->len = 0;
  x->buf[0] = '\0';
}
static void xb_free(XBuf *x) {
  free(x->buf);
  x->buf = NULL;
}

static void xb_append(XBuf *x, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (x->len + needed + 2 >= x->cap) {
    x->cap = x->len + needed + 1024 * 16;
    x->buf = (char *)realloc(x->buf, x->cap);
  }
  va_start(args, fmt);
  x->len += vsnprintf(x->buf + x->len, x->cap - x->len, fmt, args);
  va_end(args);
}

/* XML-escape a string */
static void xml_escape(const char *in, char *out, int outlen) {
  int oi = 0;
  for (int i = 0; in[i] && oi + 6 < outlen; i++) {
    switch (in[i]) {
    case '&':
      memcpy(out + oi, "&amp;", 5);
      oi += 5;
      break;
    case '<':
      memcpy(out + oi, "&lt;", 4);
      oi += 4;
      break;
    case '>':
      memcpy(out + oi, "&gt;", 4);
      oi += 4;
      break;
    case '"':
      memcpy(out + oi, "&quot;", 6);
      oi += 6;
      break;
    case '\'':
      memcpy(out + oi, "&apos;", 6);
      oi += 6;
      break;
    default:
      out[oi++] = in[i];
      break;
    }
  }
  out[oi] = '\0';
}

static const char *kind_to_iec(SymbolKind k) {
  switch (k) {
  case SYM_BOOL:
    return "BOOL";
  case SYM_INT:
    return "INT";
  case SYM_REAL:
    return "REAL";
  case SYM_TIMER:
    return "TON";
  default:
    return "BOOL";
  }
}

int export_plcopen(CompilerCtx *ctx, const char *out_path) {
  log_info(ctx, "PLCopen: generating IEC 61131-10 XML export");

  XBuf xb;
  xb_init(&xb);

  char ts[64];
  if (ctx->deterministic_output) {
    strncpy(ts, "1970-01-01T00:00:00", sizeof(ts));
    ts[sizeof(ts) - 1] = '\0';
  } else {
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    if (tm_info)
      strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm_info);
    else
      strncpy(ts, "unknown", sizeof(ts));
    ts[sizeof(ts) - 1] = '\0';
  }

  /* XML header */
  xb_append(&xb, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  xb_append(&xb, "<project xmlns=\"http://www.plcopen.org/xml/tc6_0201\">\n");
  xb_append(&xb,
            "  <fileHeader companyName=\"PLC DSL Compiler\" "
            "productName=\"plc_compiler\" productVersion=\"2.0\" "
            "creationDateTime=\"%s\"/>\n",
            ts);
  xb_append(&xb,
            "  <contentHeader name=\"PLCProgram\" "
            "modificationDateTime=\"%s\">\n",
            ts);
  xb_append(&xb, "    <addData>\n");
  xb_append(&xb, "      <data name=\"cpuProfile\" handleUnknown=\"implementation\">\n");
  xb_append(&xb, "        <cpu arch=\"%s\" bits=\"%d\" endian=\"%s\" hardening=\"%d\"/>\n",
            cpu_arch_cli_name(ctx->cpu_arch), cpu_arch_bits(ctx->cpu_arch),
            cpu_arch_endian(ctx->cpu_arch), ctx->hardening_level);
  xb_append(&xb, "      </data>\n");
  xb_append(&xb, "    </addData>\n");
  xb_append(&xb, "    <coordinateInfo>\n");
  xb_append(&xb, "      <fbd><scaling x=\"1\" y=\"1\"/></fbd>\n");
  xb_append(&xb, "      <ld><scaling x=\"1\" y=\"1\"/></ld>\n");
  xb_append(&xb, "      <sfc><scaling x=\"1\" y=\"1\"/></sfc>\n");
  xb_append(&xb, "    </coordinateInfo>\n");
  xb_append(&xb, "  </contentHeader>\n");
  xb_append(&xb, "  <types>\n");
  xb_append(&xb, "    <dataTypes/>\n");
  xb_append(&xb, "    <pous>\n");

  /* POU (Program Organization Unit) */
  xb_append(&xb, "      <pou name=\"PLC_PRG\" pouType=\"program\">\n");

  /* Interface — variable declarations */
  xb_append(&xb, "        <interface>\n");

  /* Group by direction */
  const char *sections[] = {"inputVars", "outputVars", "localVars"};
  IODirection dirs[] = {IO_INPUT, IO_OUTPUT, IO_MEMORY};

  for (int s = 0; s < 3; s++) {
    int has_vars = 0;
    for (int i = 0; i < ctx->sym_count; i++) {
      if (ctx->symbols[i].direction == dirs[s] && ctx->symbols[i].is_used &&
          ctx->symbols[i].kind != SYM_TIMER &&
          ctx->symbols[i].kind != SYM_CONST) {
        has_vars = 1;
        break;
      }
    }
    if (!has_vars)
      continue;

    xb_append(&xb, "          <%s>\n", sections[s]);
    for (int i = 0; i < ctx->sym_count; i++) {
      Symbol *sym = &ctx->symbols[i];
      if (sym->direction != dirs[s] || !sym->is_used ||
          sym->kind == SYM_TIMER || sym->kind == SYM_CONST)
        continue;
      char esc_name[128];
      xml_escape(sym->st_name, esc_name, sizeof(esc_name));
      xb_append(&xb, "            <variable name=\"%s\">\n", esc_name);
      xb_append(&xb, "              <type><%-4s/></type>\n",
                kind_to_iec(sym->kind));
      xb_append(&xb, "            </variable>\n");
    }
    xb_append(&xb, "          </%s>\n", sections[s]);
  }

  /* Timer variables */
  int has_timers = 0;
  for (int i = 0; i < ctx->sym_count; i++)
    if (ctx->symbols[i].kind == SYM_TIMER && ctx->symbols[i].is_used) {
      has_timers = 1;
      break;
    }

  if (has_timers) {
    xb_append(&xb, "          <localVars>\n");
    for (int i = 0; i < ctx->sym_count; i++) {
      Symbol *sym = &ctx->symbols[i];
      if (sym->kind != SYM_TIMER || !sym->is_used)
        continue;
      xb_append(&xb, "            <variable name=\"%s\">\n", sym->st_name);
      xb_append(&xb, "              <type><derived name=\"TON\"/></type>\n");
      xb_append(&xb, "            </variable>\n");
    }
    xb_append(&xb, "          </localVars>\n");
  }

  xb_append(&xb, "        </interface>\n");

  /* Body — embed the generated Structured Text in CDATA */
  xb_append(&xb, "        <body>\n");
  xb_append(&xb, "          <ST>\n");
  xb_append(&xb,
            "            <xhtml xmlns=\"http://www.w3.org/1999/xhtml\">\n");
  xb_append(&xb, "<![CDATA[\n");

  /* Generate the ST body in-line */
  xb_append(&xb, "(* Auto-generated by PLC DSL Compiler v2.0 *)\n");
  xb_append(&xb, "(* PLCopen XML Export — %s *)\n\n", ts);
  xb_append(&xb, "(* CPU Profile: %s, %d-bit, %s-endian *)\n",
            cpu_arch_name(ctx->cpu_arch), cpu_arch_bits(ctx->cpu_arch),
            cpu_arch_endian(ctx->cpu_arch));
  xb_append(&xb, "(* Hardening Level: %d *)\n\n", ctx->hardening_level);

  /* Generate each IF statement as ST */
  ASTNode *root = ctx->ast_root;
  for (int i = 0; i < root->child_count; i++) {
    ASTNode *stmt = root->children[i];
    if (stmt->type == NODE_IF || stmt->type == NODE_WHILE ||
        stmt->type == NODE_CASE) {
      char st_code[4096] = {0};
      st_gen_statement(ctx, st_code, sizeof(st_code), stmt, 0);
      xb_append(&xb, "%s\n", st_code);
    } else if (stmt->type == NODE_ACTION && stmt->var_name[0]) {
      char st_code[512] = {0};
      st_gen_actions(ctx, st_code, sizeof(st_code), stmt, 0);
      xb_append(&xb, "%s", st_code);
    }
  }

  xb_append(&xb, "]]>\n");
  xb_append(&xb, "            </xhtml>\n");
  xb_append(&xb, "          </ST>\n");
  xb_append(&xb, "        </body>\n");
  xb_append(&xb, "      </pou>\n");
  xb_append(&xb, "    </pous>\n");
  xb_append(&xb, "  </types>\n");
  xb_append(&xb, "  <instances>\n");
  xb_append(&xb, "    <configurations>\n");
  xb_append(&xb, "      <configuration name=\"Config\">\n");
  xb_append(&xb, "        <resource name=\"Res\">\n");
  xb_append(&xb, "          <task name=\"MainTask\" interval=\"T#10ms\" "
                 "priority=\"1\">\n");
  xb_append(&xb, "            <pouInstance name=\"PLC_PRG_Instance\" "
                 "typeName=\"PLC_PRG\"/>\n");
  xb_append(&xb, "          </task>\n");
  xb_append(&xb, "        </resource>\n");
  xb_append(&xb, "      </configuration>\n");
  xb_append(&xb, "    </configurations>\n");
  xb_append(&xb, "  </instances>\n");
  xb_append(&xb, "</project>\n");

  /* Write to file */
  if (compiler_write_output(ctx, out_path, xb.buf)) {
    log_info(ctx, "PLCopen: XML written to '%s' (%d bytes)", out_path, xb.len);
  } else {
    log_error(ctx, 0, "PLCopen: failed to write '%s'", out_path);
    xb_free(&xb);
    return 0;
  }

  if (ctx->print_generated_output && !ctx->json_mode)
    printf("\n%s\n", xb.buf);

  xb_free(&xb);
  return 1;
}
