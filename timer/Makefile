KERN_DIR = ~/Embedded/android/rk3399-android-8.1/kernel

obj-m	+= rk_timer.o

all:
	make -C $(KERN_DIR) M=`pwd` modules

.PHONY: push
push:
	adb root
	adb push rk_timer.ko /data

.PHONY: clean
clean:
	make -C $(KERN_DIR) M=`pwd` modules clean
