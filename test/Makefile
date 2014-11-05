all: gtest_test

BUILD_ROOT := $(shell pwd)/../
include ../fdw.mk

CXXFLAGS += $(GTEST_CFLAGS) $(COMMON_CFLAGS)
LDFLAGS += $(GTEST_LDFLAGS) $(GTEST_RPATH_WL) $(COMMON_LDFLAGS)

TESTS := *_test.hh

gtest_test: gtest_test.o $(FDW_OBJECTS)
	g++ -o $@ $< $(FDW_OBJS) $(PROTO_OBJECTS) $(LDFLAGS)

%.o: %.cc $(TESTS)
	g++ -c -o $@ $< $(CXXFLAGS)

clean:
	rm -f *.o gtest_test
