/* gc.h — Arena allocator for compiler allocations
 *
 * "Smart GC" for this compiler: allocate many small objects quickly (AST nodes),
 * then free everything at once at end of compilation. This avoids fragmentation
 * and makes huge inputs stable and fast.
 */

#ifndef PLC_GC_H
#define PLC_GC_H

#include <stddef.h>

typedef struct GcBlock {
    struct GcBlock *next;
    size_t cap;
    size_t used;
    unsigned char data[1];
} GcBlock;

typedef struct {
    GcBlock *head;
    size_t   default_block_size;
    size_t   total_allocated_bytes;
} GcArena;

void  gc_arena_init(GcArena *a, size_t default_block_size);
void *gc_arena_alloc(GcArena *a, size_t size, size_t align);
void  gc_arena_reset(GcArena *a);
void  gc_arena_destroy(GcArena *a);

#endif

