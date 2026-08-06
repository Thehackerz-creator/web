/**
 * atlas_runtime.c — The Atlas SoftPLC Execution Engine
 * This is the "Brain" that runs on the hardware.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#include "plc_compiler.h"

static int keep_running = 1;

void handle_sigint(int sig) {
    keep_running = 0;
}

/* 
 * THE SCAN CYCLE (The heartbeat of a PLC)
 * 1. Read Inputs (from physical hardware)
 * 2. Execute Logic (the compiled DSL)
 * 3. Write Outputs (to physical hardware)
 */
void runtime_loop(CompilerCtx *ctx) {
    printf("[Atlas Runtime] Starting Real-Time Loop (1ms Cycle)...\n");
    
    struct timespec start, end;
    long long elapsed_ns;

    while (keep_running) {
        clock_gettime(CLOCK_MONOTONIC, &start);

        /* --- STEP 1: READ INPUTS --- */
        // In a real system, we would read from EtherCAT/GPIO here.
        // For now, we simulate physical input states.

        /* --- STEP 2: EXECUTE LOGIC --- */
        // We run the "Simulation" engine as the actual "Control" engine.
        simulate_run(ctx);

        /* --- STEP 3: WRITE OUTPUTS --- */
        // We would send packets to EtherCAT slaves here.

        clock_gettime(CLOCK_MONOTONIC, &end);
        elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);

        /* Wait for the remainder of the 1ms (1,000,000 ns) cycle */
        if (elapsed_ns < 1000000) {
            usleep((1000000 - elapsed_ns) / 1000);
        } else {
            printf("[WARN] Cycle Overrun: %lld ns\n", elapsed_ns);
        }
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT, handle_sigint);

    if (argc < 2) {
        printf("Usage: ./atlas_runtime <program.dsl>\n");
        return 1;
    }

    /* 1. Initialize the Atlas Compiler Engine */
    CompilerCtx *ctx = compiler_create(PLC_CODESYS, FMT_STRUCTURED_TEXT);
    
    /* 2. Load and Compile the DSL on-the-fly */
    if (compiler_compile(ctx, argv[1], "internal_runtime.st")) {
        printf("[Atlas Runtime] Logic Loaded Successfully.\n");
        
        /* 3. Start the Industrial Execution Loop */
        runtime_loop(ctx);
    }

    printf("[Atlas Runtime] Shutting down safely...\n");
    compiler_destroy(ctx);
    return 0;
}
