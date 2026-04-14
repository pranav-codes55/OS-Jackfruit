# Makefile for Multi-Container Runtime Solution
#
# Targets:
#   all     - build user-space binaries + kernel module
#   ci      - build only user-space binaries (no kernel headers required)
#   module  - build only the kernel module
#   clean   - remove build artifacts

obj-m += monitor.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# Static linking so binaries run inside an Alpine rootfs.
# Override with WORKLOAD_LDFLAGS= for dynamic linking.
WORKLOAD_LDFLAGS ?= -static

USER_TARGETS := engine memory_hog cpu_hog io_pulse

all: $(USER_TARGETS) module

# CI target: user-space only, no sudo or kernel headers needed.
ci: WORKLOAD_LDFLAGS =
ci: $(USER_TARGETS)

module: monitor.ko

engine: engine.c monitor_ioctl.h
	gcc -O2 -Wall -Wextra -o engine engine.c -lpthread

memory_hog: memory_hog.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o memory_hog memory_hog.c

cpu_hog: cpu_hog.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o cpu_hog cpu_hog.c

io_pulse: io_pulse.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o io_pulse io_pulse.c

monitor.ko: monitor.c monitor_ioctl.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	if [ -d "$(KDIR)" ]; then $(MAKE) -C $(KDIR) M=$(PWD) clean; fi
	rm -f $(USER_TARGETS) *.o *.mod *.mod.c *.symvers *.order
	rm -f *.log
	rm -rf logs
	rm -f /tmp/mini_runtime.sock

.PHONY: all ci module clean
