#/*
# * Copyright 2019 VeriSilicon, Inc.
# *
# * Licensed under the Apache License, Version 2.0 (the "License");
# * you may not use this file except in compliance with the License.
# * You may obtain a copy of the License at
# *
# *      http://www.apache.org/licenses/LICENSE-2.0
# *
# * Unless required by applicable law or agreed to in writing, software
# * distributed under the License is distributed on an "AS IS" BASIS,
# * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# * See the License for the specific language governing permissions and
# * limitations under the License.
# */

ifneq ($(ARCH),)
arch=$(ARCH)
endif

arch ?= $(shell uname -m)
ifeq ($(arch),aarch64)
	arch=arm64
endif

cross ?= n
installpath ?=
packagename = vpe_package_${ARCH}

DLL_PATH = $(installpath)/usr/lib/vpe/
INC_PATH = $(installpath)/usr/local/include/vpe/
PKG_PATH = $(installpath)/usr/share/pkgconfig/
CFG_PATH = $(installpath)/etc/ld.so.conf.d/

.PHONY: all drivers vpi tools install uninstall clean help package

all: vpi tools

vpi:
	@echo "VPE build step - build VPI"

ifeq ($(DEBUG),y)
	@echo "Build debug VPE"
else
	@echo "Build release VPE"
endif
	make -C vpi CHECK_MEM_LEAK=y

tools:
	make -C tools

drivers:
	make -C drivers clean all install

package:
	$(shell if [ -d $(packagename)/ ]; then rm $(packagename) -rf; fi;)
	$(shell if [ ! -d $(packagename)/ ]; then mkdir $(packagename); fi;)
	cp sdk_libs/$(arch)/*.so $(packagename)/
	cp vpi/libvpi.so $(packagename)/
	$(shell if [ ! -d $(packagename)/vpe/ ]; then mkdir $(packagename)/vpe/; fi;)
	cp $(PWD)/vpi/inc/*.h $(packagename)/vpe
	cp build/install.sh $(packagename)/
	tar -czf drivers.tgz drivers/
	mv drivers.tgz $(packagename)/
	cp tools/srmtool $(packagename)/
	cp tools/*.sh $(packagename)/
	@echo "Name: libvpi" >  $(packagename)/libvpi.pc
	@echo "Description: VPE SDK lib, ARCH: $(arch)" >>  $(packagename)/libvpi.pc
	@echo "Version: 1.0" >>  $(packagename)/libvpi.pc
	@echo "Cflags: -I$(packagename)/" >> $(packagename)/libvpi.pc
	@echo "Libs: -L$(packagename)/ -lvpi"  >> $(packagename)/libvpi.pc

install:
ifeq ($(cross),y)
ifeq ($(installpath),)
	@echo "!! WARNNING !! non-compatible VPE will be installed to your host server system folder. "
	@ERROR @echo "Please use ./configure --installpath to set target VPE installation path."
endif
endif
	$(shell if [ ! -d $(DLL_PATH) ]; then mkdir $(DLL_PATH) -p; touch new; fi;)
	$(shell if [ ! -d $(INC_PATH) ]; then mkdir $(INC_PATH) -p; fi;)
	$(shell if [ ! -d $(PKG_PATH) ]; then mkdir $(PKG_PATH) -p; fi;)
	$(shell if [ ! -d $(CFG_PATH) ]; then mkdir $(CFG_PATH) -p; fi;)
	$(shell cp sdk_libs/$(arch)/*.so $(DLL_PATH) )
	$(shell cp vpi/libvpi.so $(DLL_PATH) )
	$(shell cp vpi/inc/*.h $(INC_PATH) )
	$(shell cp build/libvpi.pc $(PKG_PATH) )
	@echo "/usr/lib/vpe" > $(CFG_PATH)/vpe-$(arch).conf
ifeq ($(cross),n)
	$(shell if [ -f new ]; then /sbin/ldconfig; depmod; rm new; fi;)
endif
	@echo VPE installation was finished!

uninstall:
	@echo "VPE build step - uninstall"
	$(shell if [ -d $(DLL_PATH) ]; then rm $(DLL_PATH) -rf; fi; )
	$(shell if [ -d $(INC_PATH) ]; then rm $(INC_PATH) -rf; fi; )
	$(shell rm $(PKG_PATH)/libvpi.pc $(CFG_PATH)/vpe-$(arch).conf)
ifeq ($(cross),n)
	$(shell /sbin/ldconfig; depmod )
endif
	@echo VPE uninstallation was finished!

clean:
	make -C vpi clean
	$(shell if [ -d $(packagename)/ ]; then rm $(packagename)/ -rf; fi;)

help:
	@echo "  o make                - make VPI library"
	@echo "  o make clean          - clean VPE"
	@echo "  o make install        - install VPE"
	@echo "  o make: uninstall      - uninstall VPE"