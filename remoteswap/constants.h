#ifndef __RSWAP_CONSTANTS_H
#define __RSWAP_CONSTANTS_H

#ifndef ONE_MB
#define ONE_MB ((size_t)1024 * 1024)
#endif
#ifndef ONE_GB
#define ONE_GB ((size_t)1024 * 1024 * 1024)
#endif

#ifndef SWAP_ENTRIES_PER_GB
#define SWAP_ENTRIES_PER_GB (size_t)(ONE_GB >> PAGE_SHIFT)
#endif

// RDMA manage granularity, not the Heap Region.
#ifndef REGION_SIZE_GB
#define REGION_SIZE_GB ((size_t)4)
#endif

#ifdef MAX_REGION_NUM
#undef MAX_REGION_NUM
#endif
#define MAX_REGION_NUM 32 // up to 128GB remote memory


// number of segments, get from ibv_query_device. 
// Check the hardware information and chang the number here.
// For zion-# servers with ConnectX-3 IB, the hardware supports 32, but we can only use 30 here. Or it's not safe.
#define MAX_REQUEST_SGL 30

#endif // __RSWAP_CONSTANTS_H
