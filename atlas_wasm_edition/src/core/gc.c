/* gc.c — Arena allocator implementation */

#include "gc.h"
#include <stdlib.h>
#include <string.h>

static size_t align_up(size_t x, size_t a) {
    if (a == 0) return x;
    size_t r = x % a;
    return r ? (x + (a - r)) : x;
}

void gc_arena_init(GcArena *a, size_t default_block_size) {
    if (!a) return;
    a->head = NULL;
    a->default_block_size = default_block_size ? default_block_size : (1u << 20); /* 1MB */
    a->total_allocated_bytes = 0;
}

static GcBlock *new_block(size_t payload_cap) {
    size_t total = sizeof(GcBlock) + payload_cap;
    GcBlock *b = (GcBlock *)malloc(total);
    if (!b) return NULL;
    b->next = NULL;
    b->cap = payload_cap;
    b->used = 0;
    return b;
}

void *gc_arena_alloc(GcArena *a, size_t size, size_t align) {
    if (!a || size == 0) return NULL;
    if (align < sizeof(void *)) align = sizeof(void *);

    if (!a->head) {
        size_t cap = a->default_block_size;
        if (cap < size + align) cap = align_up(size + align, 4096);
        a->head = new_block(cap);
        if (!a->head) return NULL;
        a->total_allocated_bytes += cap;
    }

    GcBlock *b = a->head;
    size_t off = align_up(b->used, align);

    if (off + size > b->cap) {
        size_t cap = a->default_block_size;
        if (cap < size + align) cap = align_up(size + align, 4096);
        GcBlock *nb = new_block(cap);
        if (!nb) return NULL;
        nb->next = a->head;
        a->head = nb;
        a->total_allocated_bytes += cap;
        b = nb;
        off = align_up(b->used, align);
    }

    void *ptr = b->data + off;
    b->used = off + size;
    memset(ptr, 0, size);
    return ptr;
}

void gc_arena_reset(GcArena *a) {
    if (!a) return;
    for (GcBlock *b = a->head; b; b = b->next) b->used = 0;
}

void gc_arena_destroy(GcArena *a) {
    if (!a) return;
    GcBlock *b = a->head;
    while (b) {
        GcBlock *n = b->next;
        free(b);
        b = n;
    }
    a->head = NULL;
    a->default_block_size = 0;
    a->total_allocated_bytes = 0;
}

