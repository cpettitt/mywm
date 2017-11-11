CXX=clang++
CFLAGS=-g -std=c++11
LDFLAGS=-lxcb -lev

mywm: main.cpp
	$(CXX) $(CFLAGS) $(LDFLAGS) $? -o $@

.PHONY: run
run: mywm
	Xephyr -ac -screen 1920x1200 -br -reset -terminate :3 &
	rm /tmp/mywm.log
	touch /tmp/mywm.log
	tail -f /tmp/mywm.log &
	sleep 1
	DISPLAY=:3 sxhkd &
	DISPLAY=:3 ./$<

.PHONY: clean
clean:
	rm mywm
