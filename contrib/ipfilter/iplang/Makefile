#
# See the IPFILTER.LICENCE file for details on licencing.
#
#CC=gcc -Wuninitialized -Wstrict-prototypes -Werror -O
CFLAGS=-I..
CCARGS=$(DEBUG) -I. -I.. $(CFLAGS) -I$(DESTDIR) -I$(DESTDIR)/.. -I../ipsend

all: $(DESTDIR)/iplang_y.o $(DESTDIR)/iplang_l.o

$(DESTDIR)/iplang_y.o: $(DESTDIR)/iplang_y.c
	$(CC) $(CCARGS) $(LINUX) -c $(DESTDIR)/iplang_y.c -o $@

$(DESTDIR)/iplang_l.o: $(DESTDIR)/iplang_l.c
	$(CC) $(CCARGS) $(LINUX) -c $(DESTDIR)/iplang_l.c -o $@

iplang_y.o: iplang_y.c
	$(CC) $(CCARGS) $< -o $@

iplang_l.o: iplang_l.c
	$(CC) $(CCARGS) $< -o $@

$(DESTDIR)/iplang_l.c: iplang_l.l $(DESTDIR)/iplang_y.h
	lex iplang_l.l
	mv lex.yy.c $(DESTDIR)/iplang_l.c

$(DESTDIR)/iplang_y.c $(DESTDIR)/iplang_y.h: iplang_y.y
	yacc -d iplang_y.y
	mv y.tab.c $(DESTDIR)/iplang_y.c
	mv y.tab.h $(DESTDIR)/iplang_y.h

clean:
	/bin/rm -f *.o lex.yy.c y.tab.c y.tab.h
