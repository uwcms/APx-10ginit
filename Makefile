build: 10ginit

CFLAGS := $(CFLAGS)

10ginit: 10ginit.o mdio.o
	$(CXX) $(CFLAGS) -o $@ $^ -leasymem -lgpiod -lwisci2c

%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $< -std=gnu99

%.o: %.cpp
	$(CXX) -c $(CFLAGS) -o $@ $< -std=gnu++11

clean:
	-rm -f 10ginit *.o *.rpm

.PHONY: build clean rpm
