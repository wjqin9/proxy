#
# TCP Proxy Server
# ~~~~~~~~~~~~~~~~~~~
#
# Copyright (c) 2007 Arash Partow (http://www.partow.net)
# URL: http://www.partow.net/programming/tcpproxy/index.html
#
# Distributed under the Boost Software License, Version 1.0.
#


COMPILER         = -g++
OPTIMIZATION_OPT = -O3
OPTIONS          = --std=c++11 -pedantic -Wall -Werror $(OPTIMIZATION_OPT) -o
PTHREAD          = -lpthread
LINKER_OPT       = -L/usr/lib -lstdc++ $(PTHREAD) -lboost_thread -lboost_system

BUILD_LIST+=httpproxy

all: $(BUILD_LIST)

httpproxy: httpproxy.cpp
	$(COMPILER) $(OPTIONS) httpproxy httpproxy.cpp $(LINKER_OPT)

strip_bin :
	strip -s httpproxy

clean:
	rm -f core *.o *.bak *~ *stackdump *#
