# ewm - Epsilons Window Manager
include config.mk

SRC = drw.c dwm.c util.c 
OBJ = ${SRC:.c=.o}

all: options ewm ewm-msg

options:
	@echo ewm build options:
	@echo "CFLAGS   = ${CFLAGS}"
	@echo "LDFLAGS  = ${LDFLAGS}"
	@echo "CC       = ${CC}"

.c.o:
	${CC} -c ${CFLAGS} $<

${OBJ}: config.h config.mk

config.h:
	cp config.def.h $@

ewm: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

ewm-msg: dwm-msg.o
	${CC} -o $@ $< ${LDFLAGS}

clean:
	rm -f ewm ewm-msg ${OBJ} ewm-${VERSION}.tar.gz

dist: clean
	mkdir -p ewm-${VERSION}
	cp -R LICENSE Makefile README config.def.h config.mk\
		ewm.1 drw.h util.h ${SRC} transient.c ewm-${VERSION}
	tar -cf ewm-${VERSION}.tar ewm-${VERSION}
	gzip ewm-${VERSION}.tar
	rm -rf ewm-${VERSION}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f ewm ewm-msg ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/ewm
	chmod 755 ${DESTDIR}${PREFIX}/bin/ewm-msg
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	sed "s/VERSION/${VERSION}/g" < ewm.1 > ${DESTDIR}${MANPREFIX}/man1/ewm.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/ewm.1

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/ewm\
		${DESTDIR}${MANPREFIX}/man1/ewm.1

.PHONY: all options clean dist install uninstall
