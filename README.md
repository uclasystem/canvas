# Canvas

Canvas is a remote-memory data path designed to provide resource isolation for cloud applications co-running on the memory disaggregated cluster and enable the adaptive management policies for each application according to its program semantics. This repository contains the source code of Canvas client (``$repo_home_dir/remoteswap/client``), and Canvas server (``$repo_home_dir/remoteswap/server``), the configuration for the Canvas scheduler (``$repo_home_dir/remoteswap/configuration``) and the Linux kernel that Canvas needs (linux5.5). Please refer to our NSDI'23 paper, **[Canvas](https://www.usenix.org/conference/nsdi23/presentation/wang-chenxi)**, for more details.

# 1. Build & Install Canvas

## 1.1 Environments

Canvas is tested under the following setting(s):

```
Ubuntu 18.04/20.04
Linux 5.5
GCC 7.5.0/9.4.0
MLNX_OFED driver 5.0-2.1.8.0
```

Note that higher versions of MLNX-OFED driver (e.g., 5.8) may not support Linux 5.5.

## 1.2 Install Kernel

Next we will use Ubuntu 20.04 as an example to show how to build and install the kernel. It is not required but highly recommended to have the same kernel version for both CPU and memory server.

(1) Change the grub parameters (at least on CPU server)

```bash
sudo vim /etc/default/grub

# Choose the bootup kernel version as 5.5.0-canvas
GRUB_DEFAULT="Advanced options for Ubuntu>Ubuntu, with Linux 5.5.0-canvas"

# Change the value of GRUB_CMDLINE_LINUX to set transparent hugepage as madvise:
GRUB_CMDLINE_LINUX="transparent_hugepage=madvise"

# Apply the change
sudo update-grub
```

(2) Build the Kernel source code && install it.

```bash
# Change to the kernel folder:
cd Canvas/linux5.5

# In case new kernel options are prompted, press enter to use the default options.
cp config .config
sudo ./build_kernel.sh build
sudo ./build_kernel.sh install
# (optional) install built kernel only.
# sudo ./build_kernel.sh replace
sudo reboot
```

## 1.3 Install MLNX OFED Driver

**Preparations:**

Canvas is only tested on `MLNX_OFED-5.0-2.1.8.0`. Download and unzip the package according to your system version, on both CPU and memory server.

Take Ubuntu 20.04 as an example:

### 1.3.1 Download & Install the MLNX_OFED driver

```bash
# Download the MLNX OFED driver for the Ubuntu 20.04
wget https://content.mellanox.com/ofed/MLNX_OFED-5.0-2.1.8.0/MLNX_OFED_LINUX-5.0-2.1.8.0-ubuntu20.04-x86_64.tgz
tar xzf MLNX_OFED_LINUX-5.0-2.1.8.0-ubuntu20.04-x86_64.tgz
cd MLNX_OFED_LINUX-5.0-2.1.8.0-ubuntu20.04-x86_64

# Remove the incompatible libraries
sudo apt remove ibverbs-providers:amd64 librdmacm1:amd64 librdmacm-dev:amd64 libibverbs-dev:amd64 libopensm5a libosmvendor4 libosmcomp3 -y

# Install the MLNX OFED driver against the kernel 5.5.0
sudo ./mlnxofedinstall --add-kernel-support
```


### 1.3.2 Enable the *opensm* and *openibd* services

Disclaimer: This step is only required if you are using InfiniBand and want to configure your own subnet. Operations below are what we did in our small internal research-oriented cluster. Make sure you understand what you are doing before executing commands below.

(1) Enable and start the ***openibd*** service

```bash
sudo systemctl enable openibd
sudo systemctl start  openibd

# confirm the service is running and enabled:
sudo systemctl status openibd

# the log shown as:
● openibd.service - openibd - configure Mellanox devices
   Loaded: loaded (/lib/systemd/system/openibd.service; enabled; vendor preset: enabled)
   Active: active (exited) since Mon 2022-05-02 14:40:53 CST; 1min 24s ago
    
```

(2) Enable and start the ***opensmd*** service:

```bash
sudo systemctl enable opensmd
sudo systemctl start opensmd

# confirm the service status
sudo systemctl status opensmd

# the log shown as:
opensmd.service - LSB: Manage OpenSM
   Loaded: loaded (/etc/init.d/opensmd; generated)
   Active: active (running) since Mon 2022-05-02 14:53:39 CST; 10s ago

#
# Warning: you may encounter the problem:
#
opensmd.service is not a native service, redirecting to systemd-sysv-install.
Executing: /lib/systemd/systemd-sysv-install enable opensmd
update-rc.d: error: no runlevel symlinks to modify, aborting!

#
# Please refer to the **Question #1** in FAQ for how to solve this problem
#
```

### 1.3.3 Confirm the InfiniBand is available

Check the InfiniBand information

```bash
# Get the InfiniBand information
ibstat

# the log shown as:
# Adapter's stat should be Active.

	Port 1:
		State: Active
		Physical state: LinkUp
		Rate: 100
		Base lid: 3
		LMC: 0
		SM lid: 3
		Capability mask: 0x2651e84a
		Port GUID: 0x0c42a10300605e88
		Link layer: InfiniBand
```

## 1.4 Build the remoteswap data path

The user needs to build remoteswap on both CPU server and memory servers.

### 1.4.2 On memory server

Run `rswap-server`. This process must be alive all the time so either run it inside `tmux` or `screen`, or run it as a system service.

For now, you have to know the online core number of the **CPU server** first (sorry this hasn't been automated yet). You can check `/proc/cpuinfo` on the CPU server or simply get the number via `top` or `htop`.
A wrong core number will lead to crash.

```bash
./rswap-server <memory server ip> <memory server port> <memory pool size in GB> <number of cores on CPU server>
# an example: ./rswap-server 10.0.0.4 9400 48 32
```
### 1.4.3 On CPU server

Edit the parameters in `manage_rswap_client.sh.multi` under `$repo_home_dir/remoteswap/client` directory.

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

### 1.4.4 Compile & Install & Uninstall

To compile, enter `$repo_home_dir/remoteswap/client` on cpu server and `$repo_home_dir/remoteswap/server` on memory server, and just
```bash
make
```

To install, on CPU server,

```bash
sudo ./manage_rswap_client.sh.multi install
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

To uninstall, on CPU server,

```bash
sudo ./manage_rswap_client.sh.multi uninstall
```

# 2. Configuration

## 2.1 Configuations for Canvas scheduler

The remoteswap kernel module exposes several tuning knobs for users to fine tune its performance. We offer a config helper and a default configuration. To update the config,
one can enter the `$repo_home_dir/remoteswap/configure` directory, and change the parameters in ProcConfig.txt and SchedulerConfig.txt. Then

```bash
 sh apply.sh
```
For more details, please refer to `$repo_home_dir/remoteswap/README.md`.

## 2.2 Limit the memory resources of applications

Currently Canvas uses cgroup-v1 to control application's memory limit. Here is an example.

Create cgroup on the CPU server:

```bash
# Create a memory cgroup, memctl, for the applications running on the CPU server.
# $USER is the username of the account. It's "guest" here.
sudo cgcreate -t $USER -a $USER -g memory:/memctl

# Please confirm that a memory cgroup directory is built as:
/sys/fs/cgroup/memory/memctl
```

Limit local memory size on the CPU server. E.g., set local memory to 9GB:

```bash
# Please adjust the local memory size for different applications.
echo 9g > /sys/fs/cgroup/memory/memctl/memory.limit_in_bytes
```


# 3. FAQ

## Question#1  Enable *opensmd* service in Ubuntu 18.04/20.04

### Error message:

```bash
opensmd.service is not a native service, redirecting to systemd-sysv-install.
Executing: /lib/systemd/systemd-sysv-install enable opensmd
update-rc.d: error: no runlevel symlinks to modify, aborting!
```

### 1.1 Update the service start level in /etc/init.d/opensmd

The original /etc/init.d/opensmd 

```bash
  8 ### BEGIN INIT INFO
  9 # Provides: opensm
 10 # Required-Start: $syslog openibd
 11 # Required-Stop: $syslog openibd
 12 # Default-Start: null
 13 # Default-Stop: 0 1 6
 14 # Description:  Manage OpenSM
 15 ### END INIT INFO
```

Change the content in the line `Default-start` /etc/init.d/opensmd to :

```bash
12 # Default-Start: 2 3 4 5
```

### 1.2 Enable && Start the *opensmd* service

```bash
# Enable and strart the opensmd service
sudo update-rc.d opensmd remove -f
sudo systemctl enable opensmd
sudo systemctl start opensmd

# confirm the service status
sudo systemctl status opensmd

# The log shown as:
opensmd.service - LSB: Manage OpenSM
   Loaded: loaded (/etc/init.d/opensmd; generated)
   Active: active (running) since Mon 2022-05-02 14:53:39 CST; 10s ago
    
```

# 4. Acknowledgement

Thanks for all the constructive feedback that helped us with improving the performance and robustness of Canvas. We especially thanks **[Yulong Zhang](https://notenough19.github.io/zhangyulong/)**, an undergraduate student from the University of Chinese Academy of Sciences (UCAS), who improved the efficiency and fairness of the two-dimensional RDMA scheduler. For additional questions please contact us at yifanqiao@g.ucla.edu or wangchenxi@ict.ac.cn.
