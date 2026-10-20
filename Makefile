CXX ?= g++
CPPFLAGS += -Isrc $(shell pkg-config --cflags ncursesw)
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic
LDLIBS += $(shell pkg-config --libs ncursesw)
PREFIX ?= $(HOME)/.local

.PHONY: all test clean install
all: build/busb
build:
	mkdir -p $@
build/busb: src/main.cpp src/core.cpp src/ui.cpp src/core.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/main.cpp src/core.cpp src/ui.cpp -o $@ $(LDFLAGS) $(LDLIBS)
build/test-core: tests/core_test.cpp src/core.cpp src/core.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/core_test.cpp src/core.cpp -o $@ $(LDFLAGS)
test: build/test-core
	./build/test-core
install: build/busb
	install -Dm755 build/busb $(DESTDIR)$(PREFIX)/bin/busb
clean:
	rm -rf build
