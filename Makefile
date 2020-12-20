EXTRA_CFLAGS := -g -Werror

KDIR ?= /lib/modules/`uname -r`/build

target:
	$(MAKE) -C $(KDIR) M=$$PWD

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean

help:
	$(MAKE) -C $(KDIR) M=$$PWD help
