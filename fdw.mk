# BUILD_ROOT is required
# XXX EXTRA_CLEAN = $(EXTENSION)--$(EXTVERSION).sql
FDW_FILE_NAMES := expression.cc query.cc receiver_thread.cc data_handler.cc

FDW_SOURCES  := $(patsubst %.cc,$(BUILD_ROOT)/src/%.cc,$(FDW_FILE_NAMES))
ZMQ_LDFLAGS  := $(shell pkg-config --libs libzmq)
ZMQ_CFLAGS   := $(shell pkg-config --cflags libzmq) -I$(BUILD_ROOT)/common/cppzmq

PROTOBUF_LDFLAGS  := $(shell pkg-config --libs protobuf)
PROTOBUF_CFLAGS   := $(shell pkg-config --cflags protobuf)
PROTOBUF_PATH     := $(BUILD_ROOT)/common/proto/
PROTOBUF_PROTOS   := $(wildcard $(BUILD_ROOT)/common/proto/*.proto)
PROTOBUF_HEADERS  := $(patsubst %.proto,%.pb.h,$(PROTOBUF_PROTOS))

GTEST_PATH      := $(BUILD_ROOT)/common/gtest
GTEST_LIBDIR    := $(GTEST_PATH)/lib/
GTEST_INCLUDE   := $(GTEST_PATH)/include
GTEST_LIBS      :=  -lgtest
GTEST_LDFLAGS   := -L$(GTEST_LIBDIR) -L$(GTEST_LIBDIR)/.libs $(GTEST_LIBS) 
GTEST_CFLAGS    := -I$(GTEST_INCLUDE)
GTEST_RPATH     := -rpath=$(GTEST_LIBDIR)/ -rpath=$(GTEST_LIBDIR)/.libs/
GTEST_RPATH_WL  := -Wl,-rpath=$(GTEST_LIBDIR)/ -Wl,-rpath=$(GTEST_LIBDIR)/.libs/

COMMON_LDFLAGS  := $(BUILD_ROOT)/common/libcommon.a $(BUILD_ROOT)/common/proto/libproto.a
COMMON_CFLAGS   := -I$(BUILD_ROOT)/common -I$(BUILD_ROOT)/common/proto

FDW_OBJS := $(patsubst %.cc,%.o,$(FDW_SOURCES))

# FIXME on Windows
FIX_CXX_11_BUG :=
LINUX_LDFLAGS :=
ifeq ($(shell uname), Linux)
FIX_CXX_11_BUG :=  -Wl,--no-as-needed
LINUX_LDFLAGS :=  -pthread
endif

CXXFLAGS  += -std=c++11 -fPIC $(FIX_CXX_11_BUG) $(LINUX_LDFLAGS) $(COMMON_CFLAGS) $(PROTOBUF_CFLAGS) $(ZMQ_CFLAGS) $(CPPFLAGS) -g3
LDFLAGS   += $(FIX_CXX_11_BUG) $(LINUX_LDFLAGS) $(COMMON_LDFLAGS) $(PROTOBUF_LDFLAGS) $(ZMQ_LDFLAGS) -g3

