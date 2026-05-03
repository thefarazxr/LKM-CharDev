# Top-level Makefile — delegates to kernel/ and user/ sub-makes
#
# Targets:
#   make              build kernel module + user-space tools
#   make module       build only gpu_telem.ko
#   make user         build only C user-space tools
#   make clean        remove all build artefacts
#   make load         insmod gpu_telem.ko (requires sudo)
#   make unload       rmmod  gpu_telem.ko (requires sudo)
#   make reload       rmmod + insmod
#   make test         run scripts/test.sh on this machine
#   make qemu         boot QEMU test VM

.PHONY: all module user clean load unload reload test qemu

all: module user

module:
	$(MAKE) -C kernel

user:
	$(MAKE) -C user

clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C user   clean

load: module
	bash scripts/load.sh load $(LOAD_ARGS)

unload:
	bash scripts/unload.sh

reload:
	bash scripts/load.sh reload $(LOAD_ARGS)

test: all
	bash scripts/test.sh

qemu: module user
	bash scripts/run_qemu.sh
