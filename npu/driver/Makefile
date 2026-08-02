obj-m := rocket.o
rocket-y := rocket_core.o rocket_device.o rocket_drv.o rocket_gem.o rocket_job.o rocket_devfreq.o

KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
