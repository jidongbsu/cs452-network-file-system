#
# Makefile for the Linux nfs server
#
KERNEL_SOURCE=/lib/modules/$(shell uname -r)/build
MY_CFLAGS += -g -DDEBUG -O0
ccflags-y += ${MY_CFLAGS}
CC += ${MY_CFLAGS}

all: bmw

debug:
	make -C ${KERNEL_SOURCE} M=`pwd` modules
	EXTRA_CFLAGS="$(MY_CFLAGS)"

bmw:
	make -C ${KERNEL_SOURCE} M=`pwd` modules

# turns out that your module can't have the same name as your main file, or they call it a circular dependency issue;
# # at least this is true if you have multiple source files, see kvm in Linux kernel for example.
# # this is why we name the module audi, but our main file is named as audi.c, but not audi_main.c.
#

obj-m += bmw.o

bmw-objs := bmw_main.o nfssvc.o nfsfh.o vfs.o \
			   export.o proc.o xdr.o

clean:
	make -C $(KERNEL_SOURCE) M=$(PWD) clean
	rm -rf .tmp_versions/

.PHONY: all clean
