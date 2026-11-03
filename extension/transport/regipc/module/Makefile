# kbuild for the regipc module. Build with `make -C
# extension/transport/regipc/module` from the repo root, or `make` from here.
ifneq ($(KERNELRELEASE),)

obj-m += regipc.o
# single source file: no regipc-y line, or kbuild makes regipc.o depend on itself
# the ABI header is the extension's, one tree up and the way engine_regipc.c
# spells it: having the module carry its own copy is how the two drifted apart.
ccflags-y += -I$(src)/../../../include

else

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean

endif
