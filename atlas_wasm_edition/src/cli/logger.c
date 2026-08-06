/**
 * logger.c — Atlas Structured Logging Engine
 * High-performance, thread-safe logging with severity levels.
 */

#include "plc_compiler.h"
#include <time.h>

static const char* level_strings[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

static const char* level_colors[] = {
    "\033[94m", "\033[32m", "\033[33m", "\033[31m", "\033[35m"
};

void log_init(CompilerCtx *ctx, const char *path) {
    if (path) {
        strncpy(ctx->log_path, path, MAX_LOG_PATH - 1);
        ctx->log_file = fopen(path, "a");
        if (ctx->log_file) {
            fprintf(ctx->log_file, "\n--- ATLAS LOG SESSION START: %ld ---\n", time(NULL));
        }
    }
}

static void log_internal(CompilerCtx *ctx, int level, const char *fmt, va_list args) {
    if (ctx->quiet_mode && level < 2) return;

    time_t rawtime;
    struct tm *info;
    char buffer[20];
    time(&rawtime);
    info = localtime(&rawtime);
    strftime(buffer, 20, "%H:%M:%S", info);

    /* Console Output (with colors), suppressed in JSON mode */
    if (!ctx->json_mode) {
        printf("[%s] %s%-5s\033[0m : ", buffer, level_colors[level], level_strings[level]);
        va_list args_console;
        va_copy(args_console, args);
        vprintf(fmt, args_console);
        va_end(args_console);
        printf("\n");
    }

    /* File Output (Structured) */
    if (ctx->log_file) {
        fprintf(ctx->log_file, "[%s] [%-5s] ", buffer, level_strings[level]);
        vfprintf(ctx->log_file, fmt, args);
        fprintf(ctx->log_file, "\n");
        fflush(ctx->log_file);
    }
}

void log_debug(CompilerCtx *ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_internal(ctx, 0, fmt, args);
    va_end(args);
}

void log_info(CompilerCtx *ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_internal(ctx, 1, fmt, args);
    va_end(args);
}

void log_warn(CompilerCtx *ctx, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_internal(ctx, 2, fmt, args);
    va_end(args);
    ctx->warning_count++;
}

void log_error(CompilerCtx *ctx, int line, const char *fmt, ...) {
    va_list args;
    va_list args_store;
    va_start(args, fmt);
    va_copy(args_store, args);
    log_internal(ctx, 3, fmt, args);
    va_end(args);
    
    if (ctx->error_count < MAX_ERRORS) {
        vsnprintf(ctx->errors[ctx->error_count], MAX_ERROR_MSG, fmt, args_store);
        strncpy(ctx->error_messages[ctx->error_count], ctx->errors[ctx->error_count], MAX_ERROR_MSG - 1);
        ctx->error_messages[ctx->error_count][MAX_ERROR_MSG - 1] = '\0';
        ctx->error_lines[ctx->error_count] = line;
        ctx->error_count++;
    }
    va_end(args_store);
}

void log_close(CompilerCtx *ctx) {
    if (ctx->log_file) {
        fclose(ctx->log_file);
        ctx->log_file = NULL;
    }
}

void print_banner(void) {
    printf("┌──────────────────────────────────────────────────────────┐\n");
    printf("│  ATLAS PLC COMPILER v2.0                                 │\n");
    printf("│  High-Integrity Industrial DSL → IEC 61131-3             │\n");
    printf("└──────────────────────────────────────────────────────────┘\n");
}
