/**
 * Canvas defined macros.
 */
#ifndef __LINUX_SWAP_SWAP_GLOBAL_MACRO_H
#define __LINUX_SWAP_SWAP_GLOBAL_MACRO_H

// The global switch of remote swap
#define RSWAP_KERNEL_SUPPORT 1

#define ADC_MAX_NUM_CORES 96
#define ADC_CPU_FREQ 2100

/* constants */
#ifndef ONE_MB
#define ONE_MB 1048576UL    // 1024 x 1024 bytes
#endif

#ifndef ONE_GB
#define ONE_GB 1073741824UL // 1024 x 1024 x 1024 bytes
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE ((unsigned long)4096)	// bytes, use the define of kernel.
#endif

#ifndef PAGE_SHIFT
#define PAGE_SHIFT 12
#endif

#define JVM_HEAP_START_ADDR ((unsigned long)0x400000000000)
#define JVM_HEAP_SIZE_GB 48
#define JVM_HEAP_END_ADDR (JVM_HEAP_START_ADDR + JVM_HEAP_SIZE_GB * ONE_GB)

#define CANVAS_MAX_MEM 400ULL * ONE_GB

/**
 *  For UFFD prefetch
 */
#define TRIGGER_UFFD_PREFETCH_THRESHOLD 4UL // if below the threshold, goto user space prefetch
#define UFFD_SWAP_PREFETCH_NUM	8UL  // the max page number can be prefetched

#endif // end of __LINUX_SWAP_SWAP_GLOBAL_MACRO_H
