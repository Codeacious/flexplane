#!/bin/bash

CONTROLLER_IP="10.1.1.2"
DEV="eth0"

#CONTROLLER_IP="1.1.2.100"
#DEV="eth5"

TC="/sbin/tc"

./del_tc.sh

insmod fastpass.ko fastpass_debug=1 req_cost=1000000 req_bucketlen=1000000 ctrl_addr=$CONTROLLER_IP update_timer_ns=2048000 retrans_timeout_ns=3000000

echo -- lsmod after --
lsmod | grep fastpass
echo -----------------

TEXT=`cat /sys/module/fastpass/sections/.text`
DATA=`cat /sys/module/fastpass/sections/.data`
BSS=`cat /sys/module/fastpass/sections/.bss`
echo add-symbol-file $FASTPASS_DIR/src/kernel-mod/fastpass.ko $TEXT -s .data $DATA -s .bss $BSS

sudo $TC qdisc add dev $DEV root fastpass rate 12500Kbps
