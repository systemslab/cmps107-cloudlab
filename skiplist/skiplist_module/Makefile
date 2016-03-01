KDIR    ?= /lib/modules/$(shell uname -r)/build
PWD    := $(shell pwd)
KBUILD_CFLAGS += -g
MODNAME = skiplist_test

obj-m	:= $(MODNAME).o

all: kernel_modules

%.ko: %.o
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) $@

kernel_modules:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

modules_install: all
	install -m 644 $(MODNAME).ko $(KDIR)/drivers/$(MODNAME).ko
	/sbin/depmod -a

install:	modules_install

clean:
	$(RM) *.o *~ *.ko *.mod.c .*.o.cmd .*.ko.cmd
	$(RM) modules.order  Module.symvers
	$(RM) -r .tmp_versions
