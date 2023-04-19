#include "rswap_dram.h"
#include "rswap_ops.h"
#include "utils.h"

#include "rswap_scheduler.h"
#include <linux/swap_stats.h>

int rswap_frontswap_store(unsigned type, pgoff_t swap_entry_offset,
			  struct page *page)
{
	int ret = 0;

	ret = rswap_dram_write(page, swap_entry_offset << PAGE_SHIFT);
	if (unlikely(ret)) {
		pr_err("could not read page remotely\n");
		goto out;
	}
out:
	return ret;
}

int rswap_frontswap_load(unsigned type, pgoff_t swap_entry_offset,
			 struct page *page)
{
	int ret = 0;

	ret = rswap_dram_read(page, swap_entry_offset << PAGE_SHIFT);
	if (unlikely(ret)) {
		pr_err("could not read page remotely\n");
		goto out;
	}

out:
	return ret;
}

int rswap_frontswap_load_async(unsigned type, pgoff_t swap_entry_offset,
			       struct page *page)
{
	int ret = 0;

	ret = rswap_dram_read(page, swap_entry_offset << PAGE_SHIFT);
	if (unlikely(ret)) {
		pr_err("could not read page remotely\n");
		goto out;
	}

out:
	return ret;
}

int rswap_frontswap_poll_load(int cpu)
{
	return 0;
}

static void rswap_invalidate_page(unsigned type, pgoff_t offset)
{
#ifdef DEBUG_MODE_DETAIL
	pr_info("%s, remove page_virt addr 0x%lx\n", __func__,
		offset << PAGE_OFFSET);
#endif
	return;
}

static void rswap_invalidate_area(unsigned type)
{
#ifdef DEBUG_MODE_DETAIL
	pr_warn("%s, remove the pages of area 0x%x ?\n", __func__, type);
#endif
	return;
}

static void rswap_frontswap_init(unsigned type)
{
}

static struct frontswap_ops rswap_frontswap_ops = {
	.init = rswap_frontswap_init,
	.store = rswap_frontswap_store,
	.load = rswap_frontswap_load,
	.load_async = rswap_frontswap_load_async,
	.poll_load = rswap_frontswap_poll_load,
	.check_load = rswap_frontswap_check_load,
	.invalidate_page = rswap_invalidate_page,
	.invalidate_area = rswap_invalidate_area,
};

int rswap_register_frontswap(void)
{
	frontswap_register_ops(&rswap_frontswap_ops);
	pr_info("frontswap module loaded\n");
	return 0;
}

int rswap_replace_frontswap(void)
{
#ifdef RSWAP_KERNEL_SUPPORT
	frontswap_ops->init = rswap_frontswap_ops.init;
	frontswap_ops->store = rswap_frontswap_ops.store;
	frontswap_ops->load = rswap_frontswap_ops.load;
	frontswap_ops->load_async = rswap_frontswap_ops.load_async;
	frontswap_ops->poll_load = rswap_frontswap_ops.poll_load;
#else
	frontswap_ops->init = rswap_frontswap_ops.init;
	frontswap_ops->store = rswap_frontswap_ops.store;
	frontswap_ops->load = rswap_frontswap_ops.load;
	frontswap_ops->poll_load = rswap_frontswap_ops.poll_load;
#endif
	pr_info("frontswap ops replaced\n");
	return 0;
}

int rswap_client_init(char *server_ip, int server_port, int mem_size)
{
	return rswap_init_local_dram(mem_size);
}

void rswap_client_exit(void)
{
	rswap_remove_local_dram();
}