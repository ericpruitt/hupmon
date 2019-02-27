.POSIX:

CC = clang -std=c99
CFLAGS = -lutil -Weverything -Wno-disabled-macro-expansion -O3 -s \
    -D_DEFAULT_SOURCE

# CC = c99
# CFLAGS = -lutil -D_DEFAULT_SOURCE

all: hupmon

usage.h: usage.txt
	@echo "static const char usage[] = \"\\" > $@.tmp
	@sed -e 's/["\]/\\\0/g' -e 's/$$/\\n\\/' $? >> $@.tmp
	@echo '";' >> $@.tmp
	mv $@.tmp $@

hupmon: hupmon.c usage.h
	$(CC) $(CFLAGS) $< -o $@
	md5sum $@

clean:
	rm -f hupmon usage.h
