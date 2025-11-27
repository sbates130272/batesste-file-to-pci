KDIR ?= /lib/modules/$(shell uname -r)/build

PWD := $(shell pwd)
KERNEL_DIR := $(PWD)/kernel
USER_DIR := $(PWD)/user
INCLUDE_DIR := $(PWD)/include

all: modules user/test_file_to_pcie

modules:
	$(MAKE) -C $(KDIR) M=$(KERNEL_DIR) \
		EXTRA_CFLAGS="-I$(INCLUDE_DIR)" modules

user/test_file_to_pcie: user/test_file_to_pcie.c include/file_to_pcie.h
	gcc -I$(INCLUDE_DIR) -o user/test_file_to_pcie user/test_file_to_pcie.c

clean:
	$(MAKE) -C $(KDIR) M=$(KERNEL_DIR) clean
	rm -f user/test_file_to_pcie

install:
	@if [ ! -f $(KERNEL_DIR)/file_to_pcie.ko ]; then \
		echo "Error: Module not built. Run 'make modules' first (without sudo)."; \
		exit 1; \
	fi
	$(MAKE) -C $(KDIR) M=$(KERNEL_DIR) modules_install

uninstall:
	rm -f /lib/modules/$(shell uname -r)/updates/file_to_pcie.ko
	rm -f /lib/modules/$(shell uname -r)/extra/file_to_pcie.ko
	depmod -a

load:
	sudo modprobe file_to_pcie

unload:
	sudo rmmod file_to_pcie

test: user/test_file_to_pcie
	./user/test_file_to_pcie

.PHONY: all clean install uninstall load unload test modules

