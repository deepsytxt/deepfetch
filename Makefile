CXX      ?= c++
CXXFLAGS ?= -O2
PREFIX   ?= /usr/local
DESTDIR  ?=

deepfetch: deepfetch.cpp
	$(CXX) $(CXXFLAGS) -std=c++17 -pthread -o $@ deepfetch.cpp -ldl

install: deepfetch
	install -Dm755 deepfetch $(DESTDIR)$(PREFIX)/bin/deepfetch

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/deepfetch

clean:
	rm -f deepfetch

.PHONY: install uninstall clean
