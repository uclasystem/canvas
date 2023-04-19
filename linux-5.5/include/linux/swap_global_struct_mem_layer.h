/**
 * Canvas util functions.
 */
#ifndef __LINUX_SWAP_SWAP_GLOBAL_STRUCT_MEM_LAYER_H
#define __LINUX_SWAP_SWAP_GLOBAL_STRUCT_MEM_LAYER_H

// implemented in swapfile.c
extern uint64_t swap_partition_global_entry_offset(int swap_type);

int filling_prefetch_pte(struct vm_fault_prefetch *vmf_prefetch);

// time utils
/* reference cycles.
 * #1, Fix the clock cycles of CPU.
 * #2, Divided by CPU frequency to calculate the wall time.
 * 500 cycles/ 4.0GHz * 10^9 ns = 500/4.0 ns = xx ns.
 */
// This function has the most strict ordering. CPUID is used before and after
// RDTSC to prevent any possible speculation and OOO execution
static inline uint64_t get_reference_cycles(void)
{
	uint32_t cycles_high, cycles_low;
	__asm__ __volatile__("xorl %%eax, %%eax\n\t"
			     "CPUID\n\t"
			     "RDTSC\n\t"
			     "mov %%edx, %0\n\t"
			     "mov %%eax, %1\n\t"
			     "CPUID\n\t"
			     : "=r"(cycles_high), "=r"(cycles_low)::"%rax",
			       "%rbx", "%rcx", "%rdx");
	return ((uint64_t)cycles_high << 32) + (uint64_t)cycles_low;
}

// CPUID at the beginning prevent RDTSC being reordered earlier than it
// should be. Relatively looser than get_reference_cycles()
static inline uint64_t get_cycles_start(void)
{
	uint32_t cycles_high, cycles_low;
	__asm__ __volatile__("xorl %%eax, %%eax\n\t"
			     "CPUID\n\t"
			     "RDTSC\n\t"
			     "mov %%edx, %0\n\t"
			     "mov %%eax, %1\n\t"
			     : "=r"(cycles_high), "=r"(cycles_low)::"%rax",
			       "%rbx", "%rcx", "%rdx");
	return ((uint64_t)cycles_high << 32) + (uint64_t)cycles_low;
}

// More strict than get_cycles_start since "RDTSCP; read registers; CPUID"
// gurantee all instructions before are executed and all instructions after
// are not speculativly executed
// Refer to https://www.intel.com/content/dam/www/public/us/en/documents/white-papers/ia-32-ia-64-benchmark-code-execution-paper.pdf
static inline uint64_t get_cycles_end(void)
{
	uint32_t cycles_high, cycles_low;
	__asm__ __volatile__("RDTSCP\n\t"
			     "mov %%edx, %0\n\t"
			     "mov %%eax, %1\n\t"
			     "xorl %%eax, %%eax\n\t"
			     "CPUID\n\t"
			     : "=r"(cycles_high), "=r"(cycles_low)::"%rax",
			       "%rbx", "%rcx", "%rdx");
	return ((uint64_t)cycles_high << 32) + (uint64_t)cycles_low;
}

/**
 * UFFD profiling functions
 */

#ifdef UFFD_PREFETCH_LATENCY_PROFILING

#define UFFD_PROFILING_ARRAY_LEN                                               \
	(REGION_SIZE_GB * ONE_GB / PAGE_SIZE * MAX_REGION_NUM)

// defined in page_io.c
// storage overhead: 8 bytes per page.
extern uint64_t uffd_prefetch_lantecy[UFFD_PROFILING_ARRAY_LEN];

void reset_uffd_prefetch_latency_array(void);
void uffd_fault_deliver(size_t fault_on_virt_addr, uint64_t time);
void uffd_prefetch_received(size_t fault_on_virt_addr, uint64_t time);

#endif

#endif // __LINUX_SWAP_SWAP_GLOBAL_STRUCT_MEM_LAYER_H
