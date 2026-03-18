# 1. Desactivar el gadget actual
echo 0 > /sys/class/android_usb/android0/enable

# 2. Definir nuevas funciones (adb + red)
echo rndis,adb > /sys/class/android_usb/android0/functions

# 3. Reactivar
echo 1 > /sys/class/android_usb/android0/enable

ifconfig rndis0 192.168.23.100 netmask 255.255.255.0 up

route add default gw 192.168.23.1

echo "nameserver 1.1.1.1" > /tmp/resolv.conf

dropbear -R -p 22

