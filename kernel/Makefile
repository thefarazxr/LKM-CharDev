# kernel/Makefile — builds gpu_telem.ko from two source files
#
# gpu_telem.ko is a multi-object module:
#   gpu_telem_core.o  — chardev, file_operations, blocking reads, wait queue
#   ringbuf.o         — ring buffer, spinlocks, producer/consumer races
#
# The top-level include/ directory is added to the compiler search path so
# both source files can reach gpu_telem_uapi.h with a short include path.

KDIR ?= /lib/modules/$(shell uname -r)/build

# This variable is evaluated inside the kernel build system (kbuild), where
# $(src) is the absolute path to this Makefile's directory.
ccflags-y := -I$(src)/..

obj-m := gpu_telem.o
gpu_telem-y := gpu_telem_core.o ringbuf.o

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
