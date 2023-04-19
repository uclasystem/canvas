#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/inet.h>
#include <linux/init.h>

#include <linux/frontswap.h>
// #define LIMIT_SWAP_CACHE_SIZE
#ifdef LIMIT_SWAP_CACHE_SIZE
#include <linux/swap_stats.h>
#endif
#include "rswap_ops.h"
#include "utils.h"

MODULE_AUTHOR("Chenxi Wang, Yifan Qiao, Yulong Zhang");
MODULE_DESCRIPTION("RSWAP, remote memory paging over RDMA");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("1.0");

static char server_ip[INET_ADDRSTRLEN];
static int server_port;
static int remote_mem_size;

MODULE_PARM_DESC(sip, "Remote memory server ip address");
MODULE_PARM_DESC(sport, "Remote memory server port");
MODULE_PARM_DESC(rmsize, "Remote memory size in GB");
module_param_string(sip, server_ip, INET_ADDRSTRLEN, 0644);
module_param_named(sport, server_port, int, 0644);
module_param_named(rmsize, remote_mem_size, int, 0644);

int __init rswap_cpu_init(void)
{
	int ret = 0;

	ret = rswap_client_init(server_ip, server_port, remote_mem_size);
	if (unlikely(ret)) {
		pr_err("%s, rswap_rdma_client_init failed. \n", __func__);
		goto out;
	}

#ifdef RSWAP_KERNEL_SUPPORT
	if (!frontswap_enabled()) {
		ret = rswap_register_frontswap();
		if (unlikely(ret)) {
			pr_err("%s, Enable frontswap path failed. \n",
			       __func__);
			goto out;
		}
	} else {
		rswap_replace_frontswap();
	}
#else
	ret = rswap_register_frontswap();
	if (unlikely(ret)) {
		pr_err("%s, Enable frontswap path failed. \n", __func__);
		goto out;
	}
#endif

out:
	return ret;
}

void __exit rswap_cpu_exit(void)
{
	pr_info("Prepare to remove the CPU Server module.\n");
	rswap_client_exit();

	pr_info("unloading frontswap module\n");
	pr_info("1) decrease frontswap_enabled_key to 0. \n");
	pr_info("2) Remove all registered frontswap_ops from the link list.\n");

	frontswap_deregister_ops();

	pr_info("Remove CPU Server module DONE. \n");
	return;
}

module_init(rswap_cpu_init);
module_exit(rswap_cpu_exit);
