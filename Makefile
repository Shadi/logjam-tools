.PHONY: all clean install uninstall test run libs

INCLUDE_PATHS=-I/usr/local/include -I/opt/local/include
LIB_PATHS=-L/usr/local/lib -L/opt/local/lib
OPTS=-O3 -ggdb
ZLIBS=-lzmq -lczmq

FIVE=0 1 2 3 4
ULIMIT=20000

PROGRAMS=logjam-device logjam-proxy tester test_subscriber test_publisher

all: $(PROGRAMS)

logjam-device: logjam-device.c
	gcc -o logjam-device $(INCLUDE_PATHS) $(OPTS) $(LIB_PATHS) $(ZLIBS) -lrabbitmq logjam-device.c

logjam-proxy: logjam-proxy.c
	gcc -o logjam-proxy $(INCLUDE_PATHS) $(LIB_PATHS) $(OPTS) $(ZLIBS) logjam-proxy.c

tester: tester.c
	gcc -o tester $(INCLUDE_PATHS) $(LIB_PATHS) $(OPTS) $(ZLIBS) tester.c

test_subscriber: test_subscriber.c
	gcc -o test_subscriber $(INCLUDE_PATHS) $(LIB_PATHS) $(OPTS) $(ZLIBS) test_subscriber.c

test_publisher: test_publisher.c
	gcc -o test_publisher $(INCLUDE_PATHS) $(LIB_PATHS) $(OPTS) $(ZLIBS) test_publisher.c

clean:
	rm -f $(PROGRAMS) $(wildcard *.o)

install:
	install logjam-device /usr/local/bin
	install logjam-proxy /usr/local/bin

uninstall:
	rm -f /usr/local/bin/logjam-device
	rm -f /usr/local/bin/logjam-proxy

test: tester
	for i in $(FIVE); do (ulimit -n $(ULIMIT); ./tester 200 100000&); done

run: logjam-device
	ulimit -n $(ULIMIT); ./logjam-device 12345 localhost

libs:
	./install-libs
