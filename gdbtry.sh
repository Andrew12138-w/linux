#!/bin/bash


sudo qemu-system-x86_64 \
    -name "wyz_qemu" \
    -kernel /usr/src/linux/arch/x86_64/boot/bzImage \
    -initrd /boot/initrd.img-5.14.0+ \
    -enable-kvm \
    -cpu host \
    -smp 8 \
    -m 8G \
    -nographic \
    -gdb tcp::1234 \
    -append "root=/dev/sda2 earlyprintk=serial,ttyS0 console=ttyS0 nokaslr" \
    -hda /mnt/shared/u20s.qcow2 \
    -S

