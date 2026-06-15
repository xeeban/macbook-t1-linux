obj-m += t1-socw-test.o
obj-m += t1-socw-pm.o
obj-m += t1-frst-test.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
