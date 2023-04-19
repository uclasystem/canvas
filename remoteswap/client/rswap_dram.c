#include "rswap_dram.h"
#include "constants.h"

static void *local_dram; // a buffer created via vzalloc
static uint64_t local_mem_size; // local DRAM size in GB

int rswap_dram_write(struct page *page, size_t roffset)
{
	void *page_vaddr;

	page_vaddr = kmap_atomic(page);
	copy_page((void *)(local_dram + roffset), page_vaddr);
	kunmap_atomic(page_vaddr);
	return 0;
}

int rswap_dram_read(struct page *page, size_t roffset)
{
	void *page_vaddr;

	VM_BUG_ON_PAGE(!PageSwapCache(page), page);
	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(PageUptodate(page), page);

	page_vaddr = kmap_atomic(page);
	copy_page(page_vaddr, (void *)(local_dram + roffset));
	kunmap_atomic(page_vaddr);

	SetPageUptodate(page);
	unlock_page(page);
	return 0;
}

int rswap_init_local_dram(int _mem_size)
{
	local_mem_size = (uint64_t)_mem_size * ONE_GB;
	local_dram = vzalloc(local_mem_size);
	pr_info("Allocate local dram 0x%llx bytes for debug\n", local_mem_size);
	return 0;
}

int rswap_remove_local_dram(void)
{
	vfree(local_dram);
	pr_info("Free the allocated local_dram 0x%llx bytes \n",
		local_mem_size);
	return 0;
}
