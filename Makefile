CC      ?= clang
SWIFTC  ?= swiftc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS  = -ldl -framework CoreGraphics -framework IOKit -framework CoreFoundation

BIN     = brightctl2
APP     = BrightCtl.app

.PHONY: all app test clean install

all: $(BIN)

$(BIN): brightctl2.c brightctl_core.c brightctl_core.h
	$(CC) $(CFLAGS) -o $@ brightctl2.c brightctl_core.c $(LDFLAGS)

# 菜单栏应用：Swift 壳 + C 核心
app: $(APP)

$(APP): BrightCtlApp.swift brightctl_core.c brightctl_core.h BrightCtl-Info.plist BrightCtl-Bridging-Header.h
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	$(CC) $(CFLAGS) -c brightctl_core.c -o /tmp/brightctl_core.o -I.
	$(SWIFTC) -O -parse-as-library -swift-version 5 -o $(APP)/Contents/MacOS/BrightCtl \
	  BrightCtlApp.swift /tmp/brightctl_core.o \
	  -import-objc-header BrightCtl-Bridging-Header.h \
	  -framework Cocoa -framework CoreGraphics -framework IOKit -framework CoreFoundation -ldl
	cp BrightCtl-Info.plist $(APP)/Contents/Info.plist
	codesign --force --deep --sign - $(APP) 2>/dev/null || true

test: $(BIN)
	./test.sh

# 不接触硬件的关闭/恢复故障测试（CG、DisplayServices、偏好均被替换）
blank-failure-test:
	$(CC) -O0 -Wall -Wextra -I. tests/blank_failure_test.c -o /tmp/brightctl-blank-failure-test \
	  -framework CoreGraphics -framework CoreFoundation -framework IOKit -ldl
	/tmp/brightctl-blank-failure-test

install: $(APP)
	rm -rf /Applications/$(APP)
	cp -R $(APP) /Applications/
	/usr/bin/open /Applications/$(APP)

clean:
	rm -f $(BIN)
	rm -rf $(APP)
