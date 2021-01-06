KERN_DIR = ~/Embedded/android/rk3399-android-8.1/kernel

obj-m	+= rk_led.o

all:
	make -C $(KERN_DIR) M=`pwd` modules
	aarch64-linux-gcc led_app.c -o led_app.o -static

.PHONY: push
push:
	adb root
	adb push rk_led.ko /data
	adb push led_app.o /data
	adb shell chmod 777 /data/led_app.o

.PHONY: clean
clean:
	make -C $(KERN_DIR) M=`pwd` modules clean
