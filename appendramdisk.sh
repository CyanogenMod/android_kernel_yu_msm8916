#!/bin/bash
 
export curdate=`date "+%m-%d-%Y"`
 

 
cd ramdisk

echo "making boot image"
./mkbootimg --base 0x80000000 --kernel zImage --ramdisk_offset 0x01000000 --cmdline "androidboot.console=ttyHSL0 androidboot.hardware=qcom user_debug=31 msm_rtb.filter=0x3F ehci-hcd.park=3 androidboot.bootdevice=7824900.sdhci sched_enable_hmp=1" --ramdisk ramdisk.cpio.gz --dt dt.img -o boot.img
cp -vr boot.img ../out
 
echo " "
 
