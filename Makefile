EXTENSION := src/virtdb_fdw
EXTVERSION := $(shell grep default_version $(EXTENSION).control | sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

all: $(EXTENSION)--$(EXTVERSION).sql common-build-all

UNAME_S := $(shell uname -s)
BUILD_ROOT := $(shell pwd)
include ./fdw.mk

MODULE_big = virtdb_fdw
OBJS := src/virtdb_fdw_main.o src/virtdb_fdw.o
SHLIB_LINK := -lstdc++
DATA := $(EXTENSION)--$(EXTVERSION).sql
EXTRA_CLEAN := $(EXTENSION)--$(EXTVERSION).sql \
               $(wildcard $(BUILD_ROOT)/src/*.d) \
               $(wildcard $(BUILD_ROOT)/src/*.o) \
               $(OBJS) \
               $(DEPS) \
               $(wildcard $(BUILD_ROOT)/common/lib*.a) \
               $(wildcard $(BUILD_ROOT)/common/proto/lib*.a) \
               $(wildcard $(BUILD_ROOT)/common/proto/*.pb.*)
PG_CONFIG ?= $(shell which pg_config)
ifeq ($(PG_CONFIG), )
$(info $$PG_CONFIG is [${PG_CONFIG}])
PG_CONFIG = $(shell which pg_config)
endif

COMMON_LIB := $(BUILD_ROOT)/common/libcommon.a
PROTO_LIB := $(BUILD_ROOT)/common/proto/libproto.a

ifeq ($(PG_CONFIG), )
$(info $$PG_CONFIG is [${PG_CONFIG}])
$(error need pg_config executable to build the foreign data wrapper. please pass its location on the PG_CONFIG variable or make sure it can be found in the PATH)
endif

PGXS := $(shell $(PG_CONFIG) --pgxs)
PG_CPPFLAGS := $(ZMQ_CFLAGS) $(PROTOBUF_CFLAGS) $(SODIUM_CFLAGS)
PG_LIBS := -lstdc++ $(ZMQ_LDFLAGS) $(SODIUM_LDFLAGS) $(PROTOBUF_LDFLAGS) $(COMMON_LIB) $(PROTO_LIB)

include $(PGXS)

CXX ?= g++

$(COMMON_LIB) $(PROTO_LIB): $(PROTOBUF_HEADERS)

ifeq ($(RELEASE), 1)
LDFLAGS += $(PG_LIBS) -O3
OLDCFLAGS := $(CFLAGS:-O2=-O3)
override CFLAGS := $(OLDCFLAGS) -O3
else
LDFLAGS += $(PG_LIBS) -g3
OLDCFLAGS := $(CFLAGS:-O2=-O0)
override CFLAGS := $(OLDCFLAGS) -g3
endif

$(info $$CFLAGS is [${CFLAGS}])
$(info $$LDFLAGS is [${LDFLAGS}])

test-build-all:
	@echo "building tests"
	make -C test/ all

test-build-clean:
	@echo "cleaning tests"
	make -C test/ clean

common-build-all:
	cd $(BUILD_ROOT)/common; make -f common.mk all RELEASE=1

$(PROTOBUF_HEADERS): $(PROTOBUF_PROTOS)
	cd $(BUILD_ROOT)/common; make -f common.mk all

$(EXTENSION)--$(EXTVERSION).sql: $(EXTENSION).sql
	echo $< $@
	cp $< $@

%.o: %.cc $(PROTOBUF_HEADERS)
	$(CXX) -c -o $@ $< $(CXXFLAGS)
	$(CXX) -MM $*.cc -MT $@ -MF $*.d $(CXXFLAGS)

virtdb-clean: test-build-clean clean
	cd $(BUILD_ROOT)/common; make -f common.mk clean
	@echo "checking for suspicious files"
	@find . -type f -name "*.so"
	@find . -type f -name "*.a"
	@find . -type f -name "*.o"
	@find . -type f -name "*.desc"
	@find . -type f -name "*.pb.desc"
	@find . -type f -name "*.pb.h"
	@find . -type f -name "*.pb.cc"
	@find . -type f -name "*.pb.h"
