#! /bin/bash


### Parameters

version="5.5.0"
LocalVersion="-canvas"
# # Or remove the suffix
# LocalVersion=

### Operations

op=$1


if [ -z "${op}"  ]
then
	echo "Please select the operation, e.g. build, install, replace, update_grub"
	read op
fi

echo "Do the action ${op}"





# Detect Linux releases
OS_DISTRO=$( awk -F= '/^NAME/{print $2}' /etc/os-release | sed -e 's/^"//' -e 's/"$//' )
if [[ $OS_DISTRO == "CentOS Linux" ]]
then
	echo "Running on CentOS..."
elif [ $OS_DISTRO == "Ubuntu" ]
then
	echo "Running on Ubuntu..."
fi

## Functions
delete_old_kernel_contents () {
	if [[ $OS_DISTRO == "CentOS Linux" ]]
	then
		echo "sudo rm /boot/initramfs-${version}${LocalVersion}.img   /boot/System.map-${version}${LocalVersion}  /boot/vmlinuz-${version}${LocalVersion} "
		sleep 1
		sudo rm /boot/initramfs-${version}${LocalVersion}.img   /boot/System.map-${version}${LocalVersion}  /boot/vmlinuz-${version}${LocalVersion}
	elif [ $OS_DISTRO == "Ubuntu" ]
	then
		echo "sudo rm /boot/initrd.img-${version}${LocalVersion} /boot/System.map-${version}${LocalVersion} /boot/vmlinuz-${version}${LocalVersion} "
		sleep 1
		sudo rm /boot/initrd.img-${version}${LocalVersion} /boot/System.map-${version}${LocalVersion} /boot/vmlinuz-${version}${LocalVersion}
	fi
}


install_new_kernel_contents () {
	echo "install kernel modules"
	sleep 1
	sudo make -j`nproc` INSTALL_MOD_STRIP=1 modules_install

	echo "install kernel image"
	sleep 1
	sudo make install

	echo "Install uapi kernel headers to /usr/include/linux/"
	sudo make headers_install INSTALL_HDR_PATH=/usr ARCH=x86

}



update_grub_entries () {
	if [[ $OS_DISTRO == "CentOS Linux" ]]
	then
		# For CentOS, there maybe 2 grub entries
		echo "(MUST run with sudo)Delete old grub entry:"

		efi_grub="/boot/efi/EFI/centos/grub.cfg"
		if [[ -e /boot/efi/EFI/centos/grub.cfg ]]
		then
			echo " Delete EFI grub : sudo rm ${efi_grub}"
			sleep 1
			sudo rm ${efi_grub}

			echo " Rebuild EFI grub : sudo grub-mkconfig -o ${efi_grub}"
			sleep 1
			sudo grub2-mkconfig -o ${efi_grub}

		else
			echo "Delete /boot/grub/grub.cfg"
			sleep 1
			sudo rm /boot/grub/grub.cfg

			echo "Rebuild the grub.cfg"
			echo "grub-mkconfig -o /boot/grub/grub.cfg"
			sleep 1
			sudo grub2-mkconfig -o /boot/grub/grub.cfg
		fi

		echo "Set default entry to Item 0"
		sudo grub-set-default 0

		echo "Current grub entry"
		sleep 1
		sudo grub2-editenv list
	elif [ $OS_DISTRO == "Ubuntu" ]
	then
		# # Ubuntu: to list grub entries
		# awk -F\' '/menuentry / {print $2}' /boot/grub/grub.cfg
		echo " Rebuild grub"
		sudo update-grub2
	fi
}



### Do the action

if [ "${op}" = "build" ]
then
	echo "make oldconfig"
	sleep 1
#	make oldconfig

	echo "make LOCALVERSION=${LocalVersion}  -j`nproc`"
	sleep 1
	make LOCALVERSION="${LocalVersion}"  -j`nproc`

elif [ "${op}" = "install" ]
then
	delete_old_kernel_contents
	sleep 1

	install_new_kernel_contents
	sleep 1

	update_grub_entries

elif [ "${op}" = "replace"  ]
then
	delete_old_kernel_contents
	sleep 1

	echo "Install kernel image only"
	sudo make install
	sleep 1

	update_grub_entries

elif [ "${op}" = "update_grub"  ]
then

	update_grub_entries

else
	echo "!! Wrong Operation - ${op} !!"
fi
