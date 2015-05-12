default: all

all:
	gcc jenkin_mon.c -ggdb3 -O0 -lxml2 -lpthread -lrt -I/usr/include/libxml2 -o jenkin_mon

clean:
	rm -rf jenkin_mon
	rm -rf *.o
