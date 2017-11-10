CFLAGS=
LDFLAGS=-lxcb -lev

mywm: main.cpp
	$(CXX) $(CFLAGS) $(LDFLAGS) $? -o $@


run: mywm
	Xephyr -ac -screen 1920x1200 -br -reset -terminate :3 &
	rm /tmp/mywm.log
	touch /tmp/mywm.log
	tail -f /tmp/mywm.log &
	DISPLAY=:3 sxhkd &
	DISPLAY=:3 ./$<
