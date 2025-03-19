CFLAGS=`pkg-config --cflags libpng` -g
LIBS=`pkg-config --libs libpng`

PROGS=png-text-append png-text-dump png-text-add

.PHONY: all clean dist-clean

all: $(PROGS)

png-text-append: png-text-append.o crc.o
	$(CC) -o $@ $^

png-text-add: png-text-add.o crc.o
	$(CC) -o $@ $^

png-text-dump: png-text-dump.o
	$(CC) -o $@ $^ $(LIBS)


clean:
	rm -f *.o *~ *.bak

dist-clean:
	rm -f *.o $(PROGS)
