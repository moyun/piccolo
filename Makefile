CXX = g++
CC = gcc
CMAKE = cmake
#OPROFILE = 1
MAKE := $(MAKE) --no-print-directory

ifeq ($(shell which distcc > /dev/null; echo $$?), 0)
CXX := distcc $(CXX)
CC := distcc $(CC)
endif

export CXX CC CFLAGS CPPFLAGS OPROFILE

all: release debug
	cd docs/paper && $(MAKE)

bin/release/Makefile:
	@mkdir -p bin/release
	@cd bin/release && $(CMAKE) -DCMAKE_BUILD_TYPE=Release ../../src
	
bin/debug/Makefile:
	@mkdir -p bin/debug
	@cd bin/debug && $(CMAKE) -DCMAKE_BUILD_TYPE=Debug ../../src


release: bin/release/Makefile
	@cd bin/release && $(MAKE) 

debug: bin/debug/Makefile
	@cd bin/debug  && $(MAKE) 

clean:
	rm -rf bin/*

.DEFAULT: bin/debug/Makefile bin/release/Makefile
	@cd bin/release && $(MAKE) $@
	@cd bin/debug && $(MAKE) $@
	
