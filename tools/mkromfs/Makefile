CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -std=c11
ROMFS_INC = ../../src/kernel/fs

mkromfs: mkromfs.c $(ROMFS_INC)/romfs_format.h
	$(CC) $(CFLAGS) -I$(ROMFS_INC) -o $@ $<

clean:
	rm -f mkromfs

.PHONY: clean
