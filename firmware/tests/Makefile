CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined
# The BL618 ring, small enough that a test can fill it, and a gap queue small enough that
# the overflow test runs it full — the path that once announced gaps in the wrong place.
RING = -DSAMPLE_RING_BYTES=4096u -DSAMPLE_RING_GAPS=16u

host_tests: host_tests.cpp sample_ring.o ../teensy41_rtl_tcp/src/*.h ../bl618_rtl_udp/sample_ring.h
	$(CXX) $(CXXFLAGS) $(RING) -o $@ host_tests.cpp sample_ring.o

sample_ring.o: ../bl618_rtl_udp/sample_ring.c ../bl618_rtl_udp/sample_ring.h
	$(CC) $(CFLAGS) $(RING) -c -o $@ ../bl618_rtl_udp/sample_ring.c

test: host_tests
	./host_tests

clean:
	rm -f host_tests sample_ring.o

.PHONY: test clean
