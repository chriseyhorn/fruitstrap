IOS_CC = /Developer/Platforms/iPhoneOS.platform/Developer/usr/bin/gcc

all: fruitstrap

fruitstrap: fruitstrap.c
	gcc -g -o fruitstrap -framework CoreFoundation -framework MobileDevice -F/System/Library/PrivateFrameworks fruitstrap.c
