Linux kernel
============

There are several guides for kernel developers and users. These guides can
be rendered in a number of formats, like HTML and PDF. Please read
Documentation/admin-guide/README.rst first.

In order to build the documentation, use ``make htmldocs`` or
``make pdfdocs``.  The formatted documentation can also be read online at:

    https://www.kernel.org/doc/html/latest/

There are various text files in the Documentation/ subdirectory,
several of them using the Restructured Text markup notation.

Please read the Documentation/process/changes.rst file, as it contains the
requirements for building and running the kernel, and information about
the problems which may result by upgrading your kernel.

# Linux-5.5 kernel with remoteswap support

This repo is a fork of Linux kernel (version 5.4). We modified the swapping system to support swapping to remote memory on another machine via RDMA.

To use remote memory, RemoteSwap is needed as a collection of a deamon (on remote memory machine) and a client (on local CPU machine). See ``$repo_home_dir/remoteswap/`` for more details.

## Dependencies

The current version is tested and recommended on Ubuntu 18.04.
CentOS 7.5 should also work with corrosponding MLN_OFED drivers, but it is not fully tested recently.

* Hardware: Mellanox ConnectX-3 (InfiniBand)
* RDMA NIC driver: MLNX_OFED 4.9-2.2.4. [Download link for Ubuntu 18.04](https://www.mellanox.com/page/mlnx_ofed_eula?mtag=linux_sw_drivers&mrequest=downloads&mtype=ofed&mver=MLNX_OFED-4.9-2.2.4.0&mname=MLNX_OFED_LINUX-4.9-2.2.4.0-ubuntu18.04-x86_64.iso). [Download link for CentOS 7.5](https://www.mellanox.com/page/mlnx_ofed_eula?mtag=linux_sw_drivers&mrequest=downloads&mtype=ofed&mver=MLNX_OFED-4.9-2.2.4.0&mname=MLNX_OFED_LINUX-4.9-2.2.4.0-rhel7.5-x86_64.iso)

## Build & Install

### Kernel
```bash
# initialize the build config
cp config .config
# build kernel (might take a while...)
./build_kernel.sh build
# install built kernel and kernel modules
# (sudo priviledge will be needed during the script running)
./build_kernel.sh install
# (optional) install built kernel only. No need to run this after `./build_kernel.sh install'.
# This is useful when you have installed kernel modules by `./build_kernel.sh install`,
# and you want to replace only the kernel after some modifications.
# (sudo priviledge will be needed during the script running)
./build_kernel.sh replace
```

### MLNX_OFED driver

Make sure the machine is booted with the compiled kernel before installing the driver.
You might need to change the grub settings before rebooting the machine to make sure it will enter our compiled kernel.
You don't need to reinstall the driver as long as the kernel version is not changed.

```bash
# install dependencies (here is an example on Ubuntu)
sudo apt install -y python-libxml2 gfortran swig dpatch tcl quilt \
         libltdl-dev libnl-route-3-200 debhelper tk graphviz libgfortran3 \
         chrpath pkg-config
# compile and install
sudo ./mlnxofedinstall --add-kernel-support
# restart the service to enable it
sudo /etc/init.d/openibd restart
```

## Configuration

1. Build and install the kernel.
2. Update grub and change the default launching kernel to "Linux 5.5.0-canvas".
3. Reboot system, download and compile the MLNX_OFED driver
4. Deploy RemoteSwap on both local CPU server and remote memory server (follow instructions there).
5. Everything should be ready to go after RemoteSwap established the connection