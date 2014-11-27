override CFLAGS += -std=c99 -pedantic -Wall -Wextra -fPIC -g -O0
override LDFLAGS += -lz -lbz2 -llzma -lrpm -lrpmio

all: libdrpm.so.0.0.0

libdrpm.so.0.0.0: drpm.o drpm_read.o drpm_utils.o drpm_compstrm.o
	$(CC) $^ -o $@ -shared -Wl,-soname,libdrpm.so.0 $(LDFLAGS)

install:
	mkdir -p $(DESTDIR)$(libdir) $(DESTDIR)$(includedir)
	install -m 755 libdrpm.so.0.0.0 $(DESTDIR)$(libdir)/
	install -m 644 drpm.h $(DESTDIR)$(includedir)/
	ln -s $(DESTDIR)$(libdir)/libdrpm.so.0.0.0 $(DESTDIR)$(libdir)/libdrpm.so.0
	ln -s $(DESTDIR)$(libdir)/libdrpm.so.0 $(DESTDIR)$(libdir)/libdrpm.so

.PHONY: install

clean:
	rm -f *.o
