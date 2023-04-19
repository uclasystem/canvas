#ifndef __RSWAP_DRAM_H
#define __RSWAP_DRAM_H

#include <linux/vmalloc.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>

int rswap_init_local_dram(int _mem_size);
int rswap_remove_local_dram(void);
int rswap_dram_read(struct page *page, size_t roffset);
int rswap_dram_write(struct page *page, size_t roffset);

#endif // __RSWAP_DRAM_H