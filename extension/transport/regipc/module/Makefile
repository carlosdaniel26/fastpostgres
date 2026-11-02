# kbuild for the regipc module. Build with `make -C register-ipc` from the repo
# root, or `make -C module` from here.
ifneq ($(KERNELRELEASE),)

obj-m += regipc.o
# single source file: no regipc-y line, or kbuild makes regipc.o depend on itself

else

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean

endif
