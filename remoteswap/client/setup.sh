#!/bin/bash

OS_DISTRO=$( awk -F= '/^NAME/{print $2}' /etc/os-release | sed -e 's/^"//' -e 's/"$//' )

if [[ $OS_DISTRO == "CentOS Linux" ]]
then
    echo "Running on CentOS..."
    sudo chmod 777 /sys/fs/cgroup/cpuset
    sudo chmod 777 /sys/fs/cgroup/memory
    sudo chmod 777 /sys/kernel/mm/swap/vma_ra_enabled
elif [[ $OS_DISTRO == "Ubuntu" ]]
then
    echo "Running on Ubuntu..."
    sudo chmod 777 /sys/fs/cgroup/cpuset
    sudo chmod 777 /sys/fs/cgroup/memory
    sudo chmod 777 /sys/kernel/mm/swap/vma_ra_enabled
fi
