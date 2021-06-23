# Copyright (C) 2020 VeriSilicon Holdings Co., Ltd.

ifeq ($(obj),)
obj = .
endif

ifeq ($(ARCH),aarch64)
	ARCH=arm64
endif

ifeq ($(EMULATOR),y)
EXTRA_CFLAGS += -DEMULATOR
endif

ifeq ($(HW_HANDSHAKE),y)
EXTRA_CFLAGS += -DENABLE_HANDSHAKE
endif

transcoder_pcie-objs += transcoder.o pcie.o edma.o vc8000e.o bigsea.o vc8000d.o
transcoder_pcie-objs += memory.o interrupt.o debug_trace.o hw_monitor.o
transcoder_pcie-objs += encoder.o misc_ip.o

obj-m += transcoder_pcie.o

arch ?= $(shell uname -m)
ifeq ($(arch),aarch64)
EXTRA_CFLAGS += -DLS1046A
KERN_DIR ?= /root/soliosx/linux
else
KERN_DIR ?= /lib/modules/$(shell uname -r)/build
endif

DRV_PATH ?= /lib/modules/`uname -r`/kernel/drivers/pci/pcie/solios-x/
FRM_PATH ?= /lib/firmware/

all:
	$(MAKE) -C $(KERN_DIR) M=`pwd` modules

install:
	$(shell if [ ! -d $(DRV_PATH) ]; then mkdir $(DRV_PATH) -p; fi;)
	$(shell if [ ! -d $(FRM_PATH) ]; then mkdir $(FRM_PATH) -p; fi;)
	$(shell cp transcoder_pcie.ko $(DRV_PATH) )
	$(shell cp firmware/ZSP_FW_RP_V*.bin $(FRM_PATH)/transcoder_zsp_fw.bin )
ifneq ($(shell lsmod | grep transcoder_pcie), )
	@echo "VPE Driver is already installed, removing..."
	$(shell rmmod transcoder_pcie )
endif
	$(shell insmod transcoder_pcie.ko )
	@echo "VPE Driver: $(shell lsmod | grep transcoder_pcie)"

uninstall:
	$(shell rm $(FRM_PATH)/transcoder_zsp_fw.bin )
	$(shell rm $(DRV_PATH) -rf )

test:
	$(CC) -o edma_test_hugepage edma_test_hugepage.c -lhugetlbfs
	$(CC) -o edma_link_test edma_link_test.c -lhugetlbfs
	$(CC) -o mem_test mem_test.c
	$(CC) -o edma_phyaddr_test edma_phyaddr_test.c
	$(CC) -o pcie_bw_test pcie_bw_test.c -lhugetlbfs
	$(CC) -o pcie_ddr_memtest pcie_ddr_memtest.c -lhugetlbfs

.PHONY: clean
clean:
	make -C $(KERN_DIR) M=`pwd` clean
	rm -rf edma_test_hugepage mem_test edma_link_test edma_phyaddr_test pcie_bw_test pcie_ddr_memtest
