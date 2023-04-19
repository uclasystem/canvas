
# RemoteSwap
RDMA-based remote swap system for disaggregated cluster

## Ondemand and Scalable Scheduling

Use DEFINE in utils.h to select different policies of scheduler.

**ENABLE_VQUEUE**: The scheduler is enabled when the macro is defined, otherwise it is disabled.

**POLICY_SLOW_UP_SLOW_DOWN**: When the macro is defined, the number of scheduler will be increased by one when a congestion being detected, otherwise it will soar to the demand number.

**LATENCY_THRESHOLD**: When the macro is defined, scheduler will use average RDMA latency as the threshold of ondemand scheduling, instead of using total packets sent to RDMA NIC.

## How to compile

Build `remoteswap/server` on memory server,

Build `remoteswap/client` on CPU server.

Both just simply running

```bash
make
```

## How to use

### On memory server

Run `rswap-server`. This process must be alive all the time so either run it inside `tmux` or `screen`, or run it as a system service.

For now, you have to know the online core number of the **CPU server** first (sorry this hasn't been automated yet). You can check `/proc/cpuinfo` on the CPU server or simply get the number via `top` or `htop`.
A wrong core number will lead to crash.

```bash
./rswap-server <memory server ip> <memory server port> <memory pool size in GB> <number of cores on CPU server>
# an example: ./rswap-server 10.0.0.4 9400 48 32
```
### On CPU server

Edit the parameters in `manage_rswap_client.sh.single`(for single swap partition) or `manage_rswap_client.sh.multi`(for swap partition isolation) under `remoteswap/client` directory.

Here is an excerpt of the script:

```bash
# The swap file/partition size should be equal to the whole size of remote memory
# Just example, edit them according to your machine
SWAP_PARTITION_SIZE="48"

server_ip="10.0.0.4"
server_port="9400"
swap_file="/mnt/swapfile"
```

Make sure that "SWAP_PARTITION_SIZE" equal to the remote memory pool size you set when running `rswap-server`, as well as "server_ip" and "server_port" here.

Install the `rswap-client` kernel module, which will establish the connection to the server and finalize the setup.

```bash
# no swap partition isolation
./manage_rswap_client.sh.single install
# with swap partition isolation
./manage_rswap_client.sh.multi install
```

It might take a while to allocate and register all memory in the memory pool, and establish the connection. The system should have been fully set up now.

You can optionally check system log via `dmesg`. A success should look like (1 chunk is 4GB so 12 chunks are essentially 48GB remote memory):
```
rswap_request_for_chunk, Got 12 chunks from memory server.
rdma_session_connect,Exit the main() function with built RDMA conenction rdma_session_context:0xffffffffc0e34a60 .
rswap_scheduler_init starts.
rswap_scheduler_init inits vqueues.
rswap_scheduler_init, wait for configuration to launch scheduler thd.
Swap RDMA bandwidth control functions registered.
frontswap module loaded
```

Setup done!

## How to use user-defined configuration

Under the /remoteswap/configure directory, change the parameters
in ProcConfig.txt and SchedulerConfig.txt, then
just run

```bash
 sh apply.sh
```

For more details, please refer to source code: apply.c

## Explanations about configuration files

### ProcConfig.txt

Define the parameters mostly related to running applications.

+ ProcInfo: Identity header
+ AppsNum: The number of applications
+ MaxNameLength: Max length of the name of applications.
+ ProcName: The name of applications
+ ProcThreadsNum: The number of threads in each application
+ ProcCoresAllocation: The cores binded by the application threads
+ ProcWeights: The weight of applications, used by bandwidth allocation
+ ProcLatencyCritical: Whether an application is latency-critical application(1) or not(0).

### SchedulerConfig.txt

Define the parameters mostly related to the scheduler.

+ SchedulerInfo: Identity header
+ SchedulerCores: Scheduler system can dynamically start at most 4 threads to help schedule RDMA requests.
+ SchedulerThreshold(MB/s or us): When bandwidth occupation exceeds this threshold, scheduler will take over the work of sending requests. If you set this value to negative number like -x, the system will determine threshold automatley, with upper bound x. (us is for latency threshold when you define the macro **LATENCY_THRESHOLD**)
+ SchedulerPolicyBoundary: The boundary defined when to start more threads to help schedule RDMA requests. The number is related to the number of queuing requests. If you set the first number to a negative number like -1, the system will determine boundary adapatively.
+ SchedulerCheckDuration: Scheduler system will check whether scheduler is needed every 100ms if you set this parameter to 100(ms) (Warning: too short duration may lead to crash or performance overhead)
+ SchedulerPollTimes: Scheduler will poll x times before being scheduled by CPU, if you set this parameter to x.

For more details, please refer to our source code.

## Hint

Scheduler will be shuted down by default. Only if you apply your configuration for the first time after `insmod`, will it start.

## DRAM support:

RemoteSwap supports debugging using local memory as a fake "remote memory pool". In such case, there is no "remoteswap-server", and the "remoteswap-client" should be built in the following way:

```bash
# (under the remoteswap/client dir)
make BACKEND=DRAM
```

# Any Questions or Suggestions, Please Contact

zhangyulong191@mails.ucas.ac.cn