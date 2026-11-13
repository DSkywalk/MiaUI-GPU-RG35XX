#!/bin/sh
# usbnet.sh <mode>: the one way the USB port is set up. In /usr/sbin, with
# busybox-udhcpd beside it; ADB.pak and minui call it, and so do D50adbd and
# usbeth.sh.
#
#   adb     ADB only: mass storage + adb, 10d6:0c02 (the stock gadget)
#   adbnet  ADB and a USB network: rndis + adb, 10d6:0c04
#   net     USB network only, no ADB: rndis, 10d6:0c03 (for Windows' own driver)
#   off     nothing on the port: it only charges
#   link    the console-to-console cable, this end: the port stays a host
#           and usblink_host.py (beside this script) drives the other
#           console's "net" gadget, as usblink0 192.168.137.1
#   status  key=value lines for ADB.pak's screen; changes nothing
#
# Nothing runs at boot: the port starts in host mode (the kernel's USB_A_IN)
# and only charges until a mode is asked for here. A mode first switches it
# to device mode, the way the stock D50adbd does (idpin_debug, then the
# USB_A_OUT / USB_B_IN messages); without that the gadget never reaches the
# PC. "off" puts it back as it boots.
#
# The network modes give the console 192.168.137.100 and lease 192.168.137.1
# to the PC (subnet mask only: no gateway or DNS, so the PC never routes its
# own traffic through the console). That's the address Windows' Internet
# Connection Sharing gives itself, so sharing the PC's connection just works;
# the console's default route and DNS go through the PC.
#
# Changing modes live: the old way (a new gadget with adbd still running on
# the old one) could freeze the console. Here the daemons are stopped and
# waited for, the network goes down, and only then the port, with a pause for
# the PC to see it go; a port already set up as asked isn't touched at all.
#
# Every step is logged with its result to /tmp/usbnet.log, and the daemons'
# own output goes to /tmp too (udhcpd.log, adbd.log): nothing is written to
# the SD card.
#
# The link: two consoles and a C-to-C cable, one in "net" (its gadget, its
# DHCP server and 192.168.137.100, exactly as for a PC) and this one in
# "link". This kernel has no RNDIS host driver, so usblink_host.py is one, in
# user space (usbfs + a TAP interface); it takes .1, the address the other
# side would lease, and waits for the gadget, through unplugs too. No 5 V
# crosses the cable either way: each console runs on its own battery. Its
# log is /tmp/usblink_host.log; any other mode stops it.
#
# Testing without touching the real port: USBNET_DRY=1 logs the commands
# instead of running them, USBNET_SYS points at a fake sysfs tree and
# USBNET_TMP at a scratch folder. A dry run never writes the real /sys.

HERE=$(cd "$(dirname "$0")" && pwd)
SYS=${USBNET_SYS:-/sys}
TMP=${USBNET_TMP:-/tmp}
DRY=${USBNET_DRY:-0}
LOG=$TMP/usbnet.log
G=$SYS/class/android_usb/android0
MON=$SYS/monitor/usb_port/config
BB=$HERE/busybox-udhcpd
WEBMAN=$HERE/miaui-web-manager # the web file manager, on port 80 in network modes
MODE_FILE=$TMP/usb_mode # minui reads it: "adb", or "net..." for a network

CONSOLE_IP=192.168.137.100
PC_IP=192.168.137.1
NETMASK=255.255.255.0
DNS1=8.8.8.8
DNS2=1.1.1.1
VID=10d6
PID_ADB=0c02
PID_NET=0c03
PID_ADBNET=0c04
LINK_HOST=${USBLINK_HOST:-$HERE/usblink_host.py}
LINK_PID=$TMP/usblink_host.pid
LINK_LOG=$TMP/usblink_host.log

log() {
    line="$(date +%H:%M:%S) [$(cut -d' ' -f1 /proc/uptime)] $*"
    echo "$line" >> "$LOG"
}

logfile() { # a file's lines into the log, indented
    sed 's/^/  | /' "$1" >> "$LOG"
}

# A step that matters for the port, marked in the log
checkpoint() {
    log ">> $*"
}

# A command, with its output and exit code in the log. Dry runs only log it.
run() {
    log "\$ $*"
    if [ "$DRY" = 1 ]; then
        log "  (dry run: not run)"
        return 0
    fi
    "$@" > "$TMP/usbnet.out" 2>&1
    rc=$?
    [ -s "$TMP/usbnet.out" ] && logfile "$TMP/usbnet.out"
    log "  -> exit $rc"
    return $rc
}

# Read-only probes run even in a dry run: they change nothing
probe() {
    log "\$ $*"
    "$@" > "$TMP/usbnet.out" 2>&1
    logfile "$TMP/usbnet.out"
}

# sysfile <path> <value>: written, then read back into the log
sysfile() {
    if [ "$DRY" = 1 ] && [ "$SYS" = /sys ]; then
        log "  $1 <- $2 (dry run on the real sysfs: not written)"
        return 0
    fi
    if echo "$2" > "$1" 2>"$TMP/usbnet.out"; then
        log "  ${1#$SYS/} <- $2 (now: $(cat "$1" 2>/dev/null))"
    else
        log "  ${1#$SYS/} <- $2 FAILED: $(cat "$TMP/usbnet.out")"
    fi
}
sysw() { # sysw <gadget attribute> <value>
    sysfile "$G/$1" "$2"
}
gattr() { # a gadget attribute's value
    cat "$G/$1" 2>/dev/null
}

snapshot() { # what the port and the network look like right now
    log "--- state: $1"
    for f in enable functions state idVendor idProduct bDeviceClass bDeviceSubClass bDeviceProtocol \
             f_rndis/wceis f_rndis/ethaddr; do
        log "  $f = $(gattr $f)"
    done
    log "  monitor run = $(cat "$MON/run" 2>/dev/null), idpin_debug = $(cat "$MON/idpin_debug" 2>/dev/null), last event = $(cat "$MON/usb_con_msg" 2>/dev/null)"
    log "  usb_mode = $(cat "$MODE_FILE" 2>/dev/null)"
    probe ifconfig rndis0
    probe ip route
    probe sh -c "ps | grep -E '[a]dbd|[u]dhcpd|[d]ropbear|[u]sblink_host'"
}

# The mode the port is in, from the gadget itself (usb_mode is only what
# minui is told, and can be out of date)
current_mode() {
    if [ -n "$(link_pid)" ]; then echo link; return; fi
    if [ "$(gattr enable)" != 1 ] || ! device_mode; then echo off; return; fi
    case "$(gattr functions)" in
    rndis,adb|adb,rndis) echo adbnet ;;
    rndis) echo net ;;
    *adb*) echo adb ;;
    *) echo off ;;
    esac
}

device_mode() { # is the port a USB device (for a PC), not a host?
    [ "$(cat "$MON/idpin_debug" 2>/dev/null)" = 1 ] && [ "$(cat "$MON/usb_con_msg" 2>/dev/null)" = USB_B_IN ]
}

# From host mode (how it boots) to device mode, as the stock D50adbd does
to_device_mode() {
    if device_mode; then
        log "  the port is in device mode already"
        return
    fi
    checkpoint "switching the port from host to device mode"
    sysfile "$MON/idpin_debug" 1
    sysfile "$MON/usb_con_msg" USB_A_OUT
    sysfile "$MON/usb_con_msg" USB_B_IN
}

# And back, for "off": the port as it boots (D50adbd host)
to_host_mode() {
    if [ "$(cat "$MON/idpin_debug" 2>/dev/null)" = 0 ] && [ "$(cat "$MON/usb_con_msg" 2>/dev/null)" = USB_A_IN ]; then
        log "  the port is in host mode already"
        return
    fi
    checkpoint "switching the port back to host mode"
    sysfile "$MON/idpin_debug" 0
    sysfile "$MON/usb_con_msg" USB_B_OUT
    sysfile "$MON/usb_con_msg" USB_A_IN
}

# gadget <functions> <product id> <class> <subclass> <protocol>: the port goes
# down, takes the new identity, and comes back up
gadget() {
    log "--- gadget: $1 as $VID:$2, class $3/$4/$5"
    to_device_mode
    if [ "$(gattr enable)" = 1 ] && [ "$(gattr functions)" = "$1" ] && [ "$(gattr idProduct)" = "$2" ] \
       && [ "$(gattr bDeviceClass)" = "$3" ]; then
        log "  already set up like this: the port is left as it is"
        return
    fi
    if [ "$(gattr enable)" = 1 ]; then
        checkpoint "taking the port down ($(gattr functions))"
        sysw enable 0
        # the PC sees the device go, and the old functions unbind, before
        # anything about the port changes
        [ "$DRY" = 1 ] || sleep 1
    fi
    sysw iManufacturer Anbernic
    sysw iProduct RG35XX
    sysw idVendor "$VID"
    sysw idProduct "$2"
    sysw bDeviceClass "$3"
    sysw bDeviceSubClass "$4"
    sysw bDeviceProtocol "$5"
    case "$1" in
    *rndis*)
        # wceis: the RNDIS class Windows' built-in driver looks for (E0/01/03)
        # instead of the generic CDC one (02/02/FF) it ignores. No fixed MAC:
        # this kernel makes up both ends' (g_android.host_addr, read-only,
        # would take one from the boot command line only), and f_rndis/ethaddr
        # just reports the PC's.
        sysw f_rndis/wceis 1
        sysw f_rndis/manufacturer Anbernic
        sysw f_rndis/vendorID "$VID"
        ;;
    *)
        sysw f_rndis/wceis 0
        ;;
    esac
    sysw functions "$1"
    checkpoint "bringing the port up ($1)"
    sysw enable 1
}

# The kernel's USB monitor re-runs usbmond.sh when the cable comes and goes,
# which would put its own gadget back over ours: keep it off
monitor_off() {
    sysfile "$MON/run" 0
}

# Nothing at boot brings up the loopback. adbd can't start without it once
# any address has existed since boot (a network mode's rndis0): on this
# kernel, binding its 127.0.0.1:5037 only works while no address ever has,
# and then fails with "Cannot assign requested address", so adbd quits at
# once, before it even opens the port (going from "net" back to an ADB mode)
loopback_up() {
    if ip addr show lo 2> /dev/null | grep -q "inet 127.0.0.1"; then
        log "  lo is up"
        return
    fi
    run ifconfig lo 127.0.0.1 netmask 255.0.0.0 up
}

# Dropbear looks up the client's name, and with the console's DNS out
# through a PC that doesn't share its connection, that waits 10 s for no
# answer before every login prompt. The PC is always 192.168.137.1: in
# /etc/hosts (read-only, so a copy from /tmp bound over it), the answer is
# immediate (nsswitch.conf already reads files before dns)
pc_in_hosts() {
    if grep -q "^$PC_IP[[:space:]]" /etc/hosts 2> /dev/null; then
        log "  $PC_IP is in /etc/hosts already"
        return
    fi
    log "  $PC_IP usb-pc into /etc/hosts (a copy in $TMP/hosts bound over it)"
    [ "$DRY" = 1 ] && return
    { cat /etc/hosts; printf "%s\tusb-pc\n" "$PC_IP"; } > "$TMP/hosts"
    run mount --bind "$TMP/hosts" /etc/hosts
}

# wait_gone <process name> <tenths of a second>: until none is left
wait_gone() {
    [ "$DRY" = 1 ] && return 0
    n=0
    while pidof "$1" > /dev/null; do
        if [ $n -ge "$2" ]; then
            log "  !! $1 still running after $(($2 * 100)) ms"
            return 1
        fi
        sleep 0.1
        n=$((n + 1))
    done
    log "  $1 gone after $((n * 100)) ms"
}

# stop <process name>: asked to go, then made to
stop() {
    pidof "$1" > /dev/null || { log "  $1: not running"; return 0; }
    run killall "$1"
    wait_gone "$1" 20 && return 0
    run killall -9 "$1"
    wait_gone "$1" 10
}

stop_net() {
    log "--- stopping the network"
    stop busybox-udhcpd
    rm -f "$TMP/udhcpd.pid"
    stop dropbear
    stop miaui-web-manager
    # down, so nothing is sent through the gadget while it goes (its routes go too)
    if [ -e "$SYS/class/net/rndis0" ]; then run ifconfig rndis0 down; fi
}

stop_adb() {
    log "--- stopping adbd"
    stop adbd
}

start_adb() {
    log "--- starting adbd"
    log "\$ adbd &"
    if [ "$DRY" = 1 ]; then
        log "  (dry run: not run)"
        return
    fi
    # it can die at once while the port's new functions settle: a few tries
    # (sleep is a shell builtin here, cut short when a child dies, so the
    # waits count tenths)
    : > "$TMP/adbd.log"
    for try in 1 2 3; do
        (trap - HUP PIPE; exec adbd >> "$TMP/adbd.log" 2>&1) &
        n=0
        while [ $n -lt 15 ]; do
            sleep 0.1
            n=$((n + 1))
        done
        if pidof adbd > /dev/null; then
            log "  adbd running, pid $(pidof adbd) (try $try)"
            return 0
        fi
        log "  !! adbd did not start (try $try)"
        [ -s "$TMP/adbd.log" ] && logfile "$TMP/adbd.log"
    done
    probe sh -c "dmesg | grep -iE 'adb|android' | tail -n 10"
    return 1
}

start_net() {
    log "--- starting the network"
    # rndis0 shows up a moment after the gadget is enabled
    i=0
    while [ ! -e "$SYS/class/net/rndis0" ] && [ $i -lt 50 ]; do
        sleep 0.1
        i=$((i + 1))
    done
    if [ -e "$SYS/class/net/rndis0" ]; then
        log "  rndis0 is there after $((i * 100)) ms"
    else
        log "  !! no rndis0 after 5 s: the PC hasn't picked up the RNDIS gadget"
        probe sh -c "dmesg | grep -iE 'usb|rndis|android|gadget|ether|adb' | tail -n 25"
        return 1
    fi

    run ifconfig rndis0 "$CONSOLE_IP" netmask "$NETMASK" up
    # Internet through the PC (when it shares its connection)
    run route add default gw "$PC_IP" dev rndis0
    log "  DNS: $DNS1, $DNS2 into $TMP/resolv.conf (/etc/resolv.conf links there)"
    [ "$DRY" = 1 ] || printf "nameserver %s\nnameserver %s\n" "$DNS1" "$DNS2" > "$TMP/resolv.conf"

    # the PC's address: one lease, the subnet mask and nothing else
    cat > "$TMP/udhcpd.conf" <<EOF
start $PC_IP
end $PC_IP
interface rndis0
max_leases 1
lease_file $TMP/udhcpd.leases
pidfile $TMP/udhcpd.pid
auto_time 0
offer_time 30
decline_time 60
conflict_time 60
min_lease 60
option subnet $NETMASK
option lease 86400
EOF
    log "  udhcpd.conf:"
    logfile "$TMP/udhcpd.conf"
    [ "$DRY" = 1 ] || : > "$TMP/udhcpd.leases"
    # in the foreground (behind &) so its -vv trace goes to its own log, and
    # to syslog (/tmp/messages) too
    log "\$ $BB udhcpd -f -S -vv $TMP/udhcpd.conf > $TMP/udhcpd.log 2>&1 &"
    if [ "$DRY" = 1 ]; then
        log "  (dry run: not run)"
    else
        (trap - HUP PIPE; exec "$BB" udhcpd -f -S -vv "$TMP/udhcpd.conf" > "$TMP/udhcpd.log" 2>&1) &
        sleep 1
        if pidof busybox-udhcpd > /dev/null || [ -s "$TMP/udhcpd.pid" ]; then
            log "  udhcpd running, pid $(cat "$TMP/udhcpd.pid" 2>/dev/null)"
        else
            log "  !! udhcpd did not start:"
            logfile "$TMP/udhcpd.log"
        fi
    fi

    # the web manager (port 80): its own process, no output kept
    if [ -x "$WEBMAN" ]; then
        log "\$ $WEBMAN > /dev/null 2>&1 < /dev/null &"
        if [ "$DRY" = 1 ]; then
            log "  (dry run: not run)"
        else
            (trap - HUP PIPE; exec "$WEBMAN" > /dev/null 2>&1 < /dev/null) &
        fi
    fi

    # SSH/SFTP on the console (root / linux). Its own host keys, not -R: that
    # offers every key type and makes the missing one (RSA) when a client
    # picks it, 7-20 s at this clock and again on every login, as /etc
    # can't keep it
    pc_in_hosts
    K=/etc/dropbear/dropbear
    if [ -f ${K}_ed25519_host_key ] && [ -f ${K}_ecdsa_host_key ]; then
        run dropbear -r ${K}_ed25519_host_key -r ${K}_ecdsa_host_key -p 22
    else
        run dropbear -R -p 22
    fi

    # A PC with a fixed .1 (Windows ICS, NetworkManager "shared") never asks
    # for a lease, so the Quick Menu finds it in the ARP table instead; one
    # ping puts it there. In the background: up to 30 tries while the PC sets
    # its side up, and it stops at the first reply.
    log "\$ ping -c 1 -W 1 $PC_IP, in the background until it answers (30 tries)"
    if [ "$DRY" = 1 ]; then
        log "  (dry run: not run)"
    else
        (trap - HUP PIPE
         i=0
         while [ $i -lt 30 ]; do
             ping -c 1 -W 1 "$PC_IP" && break
             sleep 1
             i=$((i + 1))
         done) > /dev/null 2>&1 < /dev/null &
    fi
}

# The link's bridge: its pid if it's running (the pid file alone could name
# some other process by now)
link_pid() {
    pid=$(cat "$LINK_PID" 2> /dev/null)
    [ -n "$pid" ] && grep -q usblink_host "/proc/$pid/cmdline" 2> /dev/null && echo "$pid"
}

stop_link() {
    pid=$(link_pid)
    if [ -z "$pid" ]; then
        rm -f "$LINK_PID"
        return 0
    fi
    log "--- stopping the console link (usblink_host, pid $pid)"
    # SIGTERM: it gives the other console's interfaces back and closes usblink0
    run kill "$pid"
    [ "$DRY" = 1 ] && return 0
    n=0
    while [ -n "$(link_pid)" ]; do # a zombie has no command line: gone
        if [ $n -ge 50 ]; then
            log "  !! still running after 5 s"
            run kill -9 "$pid"
            break
        fi
        sleep 0.1
        n=$((n + 1))
    done
    [ $n -lt 50 ] && log "  gone after $((n * 100)) ms"
    rm -f "$LINK_PID"
    [ -s "$LINK_LOG" ] && logfile "$LINK_LOG"
}

start_link() {
    log "--- starting the console link"
    if [ -n "$(link_pid)" ]; then
        log "  usblink_host is running already, pid $(link_pid)"
        return 0
    fi
    if [ ! -f "$LINK_HOST" ]; then
        log "  !! no $LINK_HOST"
        return 1
    fi
    log "\$ python3 -u $LINK_HOST --ip $PC_IP/24 --quiet > $LINK_LOG 2>&1 &"
    if [ "$DRY" = 1 ]; then
        log "  (dry run: not run)"
        return 0
    fi
    # its own pid: exec in the background subshell
    (trap - HUP PIPE; exec python3 -u "$LINK_HOST" --ip "$PC_IP/24" --quiet > "$LINK_LOG" 2>&1) &
    echo $! > "$LINK_PID"
    sleep 1
    if [ -n "$(link_pid)" ]; then
        log "  usblink_host running, pid $(link_pid); it waits for the other console"
        logfile "$LINK_LOG"
    else
        log "  !! usblink_host ended at once:"
        logfile "$LINK_LOG"
        rm -f "$LINK_PID"
        return 1
    fi
}

# The gadget off and the port back as it boots (a host): "off", and "link"
port_off() {
    if [ "$(gattr enable)" = 1 ]; then checkpoint "taking the port down"; fi
    sysw enable 0
    [ "$DRY" = 1 ] || sleep 1
    to_host_mode
}

status() {
    mode=$(current_mode)
    echo "mode=$mode"
    pidof adbd > /dev/null && echo "adbd=1" || echo "adbd=0"
    dev=rndis0
    [ "$mode" = link ] && dev=usblink0
    ip=$(ip -4 addr show dev $dev 2>/dev/null | awk '/inet / { sub("/.*", "", $2); print $2; exit }')
    echo "ip=${ip:-}"
    pc=$("$BB" dumpleases -f "$TMP/udhcpd.leases" 2>/dev/null | awk 'NR > 1 { print $2; exit }')
    echo "pc=${pc:-}"
    gw=$(ip route 2>/dev/null | awk '/^default/ { print $3; exit }')
    echo "gw=${gw:-}"
}

case "$1" in
status)
    status
    exit 0
    ;;
adb|adbnet|net|off|link) ;;
*)
    echo "usage: $0 adb|adbnet|net|off|link|status" >&2
    exit 2
    ;;
esac

# Started from an adb shell or over SSH, the session goes away when adbd or
# dropbear is stopped: that must not stop this halfway through
trap '' HUP PIPE

# One at a time
LOCK=$TMP/usbnet.lock
if ! mkdir "$LOCK" 2> /dev/null; then
    other=$(cat "$LOCK/pid" 2> /dev/null)
    if [ -n "$other" ] && kill -0 "$other" 2> /dev/null; then
        echo "usbnet.sh: already running (pid $other)" >&2
        exit 4
    fi
    rm -rf "$LOCK"
    mkdir "$LOCK"
fi
echo $$ > "$LOCK/pid"
trap 'rm -rf "$LOCK"' EXIT

: > "$LOG"
log "=== usbnet.sh $1 (dry run: $DRY, sysfs: $SYS)"

from=$(current_mode)

snapshot before
probe sh -c "lsmod 2>/dev/null"

if [ "$from" != off ] && [ "$from" != "$1" ]; then checkpoint "live switch from $from to $1"; fi

monitor_off
loopback_up
stop_net
# the link's bridge lets go of the port before anything else touches it
[ "$1" = link ] || stop_link
# adbd goes before its port does, and comes back on the new one
if [ "$1" != "$from" ] || [ "$1" = off ]; then stop_adb; fi

rc=0
case "$1" in
adb)
    gadget mass_storage,adb "$PID_ADB" 0 0 0
    pidof adbd > /dev/null || start_adb || rc=1
    [ "$DRY" = 1 ] || echo adb > "$MODE_FILE"
    ;;
adbnet)
    # the IAD class (EF/02/01): Windows keeps RNDIS's two interfaces together
    # (bDeviceClass is decimal in this sysfs: 239 = 0xEF)
    gadget rndis,adb "$PID_ADBNET" 239 2 1
    pidof adbd > /dev/null || start_adb || rc=1
    start_net || rc=1
    [ "$DRY" = 1 ] || echo net > "$MODE_FILE"
    ;;
net)
    gadget rndis "$PID_NET" 239 2 1
    start_net || rc=1
    [ "$DRY" = 1 ] || echo net-only > "$MODE_FILE"
    ;;
off)
    port_off
    [ "$DRY" = 1 ] || rm -f "$MODE_FILE"
    ;;
link)
    port_off
    start_link || rc=1
    # minui reads it as no USB mode (the port only charges, for it)
    [ "$DRY" = 1 ] || echo link > "$MODE_FILE"
    ;;
esac

sleep 1
snapshot after
probe sh -c "dmesg | grep -iE 'usb|rndis|android|gadget|ether|adb' | tail -n 15"
log "=== done: $1 (exit $rc)"
exit $rc
