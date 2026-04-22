#include "buddy.h"
#include <stddef.h>

#define PAGE_SIZE 4096
#define MAX_RANK 16
#define MIN_RANK 1
#define MAX_PAGES 32768

/* Free list heads for each rank (1..16). */
static struct free_head {
    struct free_head *next;
} *free_heads[MAX_RANK + 1];

/* Count of free blocks per rank for O(1) query_page_counts */
static int free_counts[MAX_RANK + 1];

/* Pool boundaries for address validation */
static void *pool_start;
static void *pool_end;
static int total_pages;

/* page_owner[p] = 0 if free, rank (>0) if first page of allocated block,
   -1 if non-first page of allocated block */
static char page_owner[MAX_PAGES];

/* Helper: 2^(n-1) for rank n */
static int rank_to_pages(int rank) {
    return 1 << (rank - 1);
}

/* Helper: check if address p is a valid page-aligned address within pool */
static int is_valid_page(void *p) {
    if (p < pool_start || p >= pool_end)
        return 0;
    long offset = (unsigned long)(p - pool_start);
    if (offset % PAGE_SIZE != 0)
        return 0;
    return 1;
}

/* Helper: get page index from address */
static int addr_to_page_idx(void *p) {
    return (int)((unsigned long)(p - pool_start) / PAGE_SIZE);
}

/* Helper: get address from page index */
static void *page_idx_to_addr(int idx) {
    return (void *)((unsigned long)pool_start + (unsigned long)idx * PAGE_SIZE);
}

/* Helper: check if a block of given rank starting at page idx is within pool */
static int block_fits(int idx, int rank) {
    int npages = rank_to_pages(rank);
    return idx + npages <= total_pages;
}

/* Remove a free block from its rank's free list */
static void remove_from_free_list(struct free_head *block, int rank) {
    struct free_head **pp = &free_heads[rank];
    while (*pp) {
        if (*pp == block) {
            *pp = (*pp)->next;
            free_counts[rank]--;
            return;
        }
        pp = &((*pp)->next);
    }
}

/* Add a block to the front of a rank's free list */
static void add_to_free_list(struct free_head *block, int rank) {
    block->next = free_heads[rank];
    free_heads[rank] = block;
    free_counts[rank]++;
}

/* Find the buddy of a block at page index idx with given rank.
   Buddy is at idx ^ (1 << (rank-1)) pages away. */
static int find_buddy_idx(int idx, int rank) {
    int block_size = rank_to_pages(rank);
    return idx ^ block_size;
}

/* Check if a buddy can be merged: must be within pool, free, and same rank */
static int buddy_is_mergeable(int idx, int buddy_idx, int rank) {
    if (!block_fits(buddy_idx, rank))
        return 0;
    /* The buddy must be free (first page of a free block) */
    if (page_owner[buddy_idx] != 0)
        return 0;
    return 1;
}

int init_page(void *p, int pgcount) {
    int i;

    pool_start = p;
    pool_end = (void *)((unsigned long)p + (unsigned long)pgcount * PAGE_SIZE);
    total_pages = pgcount;

    /* Initialize free lists and counts */
    for (i = MIN_RANK; i <= MAX_RANK; i++) {
        free_heads[i] = NULL;
        free_counts[i] = 0;
    }

    /* Initialize page owner array */
    for (i = 0; i < pgcount; i++)
        page_owner[i] = 0;

    /* Calculate the largest rank that fits in pgcount pages */
    int max_rank = MAX_RANK;
    while (rank_to_pages(max_rank) > pgcount)
        max_rank--;

    /* Add the entire pool as free blocks, using largest possible blocks */
    int remaining = pgcount;
    int offset = 0;
    while (remaining > 0) {
        int r = max_rank;
        while (rank_to_pages(r) > remaining)
            r--;
        add_to_free_list((struct free_head *)((unsigned long)p + (unsigned long)offset * PAGE_SIZE), r);
        offset += rank_to_pages(r);
        remaining -= rank_to_pages(r);
    }

    return OK;
}

void *alloc_pages(int rank) {
    int r, idx, npages, i;

    if (rank < MIN_RANK || rank > MAX_RANK)
        return ERR_PTR(-EINVAL);

    /* Find smallest available rank >= requested rank */
    for (r = rank; r <= MAX_RANK; r++) {
        if (free_heads[r] != NULL)
            break;
    }
    if (r > MAX_RANK)
        return ERR_PTR(-ENOSPC);

    /* Remove block from free list */
    struct free_head *block = free_heads[r];
    free_heads[r] = block->next;
    free_counts[r]--;

    /* Split until we reach the desired rank */
    while (r > rank) {
        r--;
        int block_pages = rank_to_pages(r);
        struct free_head *buddy = (struct free_head *)((unsigned long)block + (unsigned long)block_pages * PAGE_SIZE);
        add_to_free_list(buddy, r);
    }

    /* Mark as allocated */
    idx = addr_to_page_idx(block);
    npages = rank_to_pages(rank);
    for (i = 0; i < npages; i++)
        page_owner[idx + i] = (i == 0) ? rank : -1;

    return block;
}

int return_pages(void *p) {
    int idx, rank, npages, i, cur_idx, cur_rank, buddy_idx;

    if (p == NULL)
        return -EINVAL;
    if (!is_valid_page(p))
        return -EINVAL;

    idx = addr_to_page_idx(p);
    if (page_owner[idx] <= 0)
        return -EINVAL; /* Not allocated */

    rank = page_owner[idx];
    npages = rank_to_pages(rank);

    /* Clear page owner for all pages in this block */
    for (i = 0; i < npages; i++)
        page_owner[idx + i] = 0;

    /* Try to coalesce with buddy */
    cur_idx = idx;
    cur_rank = rank;

    while (cur_rank < MAX_RANK) {
        buddy_idx = find_buddy_idx(cur_idx, cur_rank);
        if (!buddy_is_mergeable(cur_idx, buddy_idx, cur_rank))
            break;

        /* Remove buddy from its free list */
        remove_from_free_list((struct free_head *)page_idx_to_addr(buddy_idx), cur_rank);

        /* Merge: the lower address becomes the new block */
        if (buddy_idx < cur_idx)
            cur_idx = buddy_idx;

        cur_rank++;
    }

    add_to_free_list((struct free_head *)page_idx_to_addr(cur_idx), cur_rank);

    return OK;
}

int query_ranks(void *p) {
    int idx, r;
    struct free_head *fh;

    if (!is_valid_page(p))
        return -EINVAL;

    idx = addr_to_page_idx(p);

    /* If the page is allocated, return its rank */
    if (page_owner[idx] > 0)
        return page_owner[idx];

    /* If the page is a non-first page of an allocated block */
    if (page_owner[idx] < 0)
        return -EINVAL; /* Not a valid start of a block */

    /* Free page: find the rank of the free block it belongs to */
    for (r = MAX_RANK; r >= MIN_RANK; r--) {
        fh = free_heads[r];
        while (fh) {
            int block_start = addr_to_page_idx(fh);
            int npages = rank_to_pages(r);
            if (idx >= block_start && idx < block_start + npages)
                return r;
            fh = fh->next;
        }
    }

    return -EINVAL;
}

int query_page_counts(int rank) {
    if (rank < MIN_RANK || rank > MAX_RANK)
        return -EINVAL;

    return free_counts[rank];
}
