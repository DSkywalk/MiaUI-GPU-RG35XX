#!/bin/sh

usb_gandroid0_enable="/sys/class/android_usb/android0/enable"
usb_gandroid0_idVendor="/sys/class/android_usb/android0/idVendor"
usb_gandroid0_idProduct="/sys/class/android_usb/android0/idProduct"
usb_gandroid0_functions="/sys/class/android_usb/android0/functions"
usb_gandroid0_lun0="/sys/class/android_usb/android0/f_mass_storage/lun0/file"
usb_gandroid0_lun1="/sys/class/android_usb/android0/f_mass_storage/lun1/file"
usb_gandroid0_iManufacturer="/sys/class/android_usb/android0/iManufacturer"
usb_gandroid0_iProduct="/sys/class/android_usb/android0/iProduct"

echo "------usbmond shell: $1"

# MiaUI is a gaming handheld first: plugged in, the port only charges. No
# gadget and no adbd until usbnet.sh is asked for a mode, and a
# mode it has set up (/tmp/usb_mode) is left alone when the cable comes and goes.
usbnet_mode=$(cat /tmp/usb_mode 2>/dev/null)

if [ $1 = USB_B_IN ];then
	echo "------USB_B_IN: device mode" > /dev/console
	
	echo "USB_B_IN" > /sys/monitor/usb_port/config/usb_con_msg

	if [ -n "$usbnet_mode" ];then
		echo "------USB_B_IN: keeping usbnet.sh mode $usbnet_mode" > /dev/console
	else
		echo '0' > $usb_gandroid0_enable
		echo "------USB_B_IN: charge only (ADB.pak sets up ADB or a network)" > /dev/console
	fi
fi

if [ $1 = USB_B_OUT ];then
	# in case that adbd has been started, unless usbnet.sh's mode wants it
	if [ -z "$usbnet_mode" ];then
		echo "------adb stop in B_OUT" > /dev/console
		killall adbd
		sleep 1
	fi

	echo "USB_B_OUT" > /sys/monitor/usb_port/config/usb_con_msg
	echo "------USB_B_OUT: device mode" > /dev/console

	echo "" > /sys/class/android_usb/android0/f_mass_storage/lun0/file
	echo "" > /sys/class/android_usb/android0/f_mass_storage/lun1/file
fi

if [ $1 = USB_A_IN ];then
	echo "USB_A_IN" > /sys/monitor/usb_port/config/usb_con_msg
fi

if [ $1 = USB_A_OUT ];then
	echo "USB_A_OUT" > /sys/monitor/usb_port/config/usb_con_msg
fi

# mount disk/partition to PC
if [ $1 = ADD_LUN ];then
X	echo "------usbmond shell: $1 $2"
	if [ $2 = LUN0 ];then
		echo $3 > $usb_gandroid0_lun0
	elif [ $2 = LUN1 ];then
		echo $3 > $usb_gandroid0_lun1
	elif [ $2 = LUN2 ];then
		echo $3 > $usb_gandroid0_lun0
		echo $4 > $usb_gandroid0_lun1
	fi
fi

# umount disk/partition to PC
if [ $1 = REMOVE_LUN ];then
	echo "------usbmond shell: $1 $2"
	if [ $2 = LUN0 ];then
		echo  > $usb_gandroid0_lun0
	elif [ $2 = LUN1 ];then
		echo  > $usb_gandroid0_lun1
	fi
fi


