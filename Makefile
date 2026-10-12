obj-m := lumenrx.o
KDIR ?= /lib/modules/$(shell uname -r)/build
OUT := $(CURDIR)/dist

# A clang-built kernel (CachyOS LTO) needs: make LLVM=1
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(OUT) modules

clean:
	rm -rf $(OUT)

test:
	mkdir -p $(OUT)
	$(CC) -Wall -Wextra -o $(OUT)/lumenrx-test lumenrx.c && $(OUT)/lumenrx-test

.PHONY: all clean test
