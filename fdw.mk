# BUILD_ROOT is required
# XXX EXTRA_CLEAN = $(EXTENSION)--$(EXTVERSION).sql

ZMQ_LDFLAGS  := $(shell pkg-config --libs libzmq)
ZMQ_CFLAGS   := $(shell pkg-config --cflags libzmq) -I$(BUILD_ROOT)/common/cppzmq

SODIUM_LDFLAGS  := $(shell pkg-config --libs libsodium) -lsodium -L$(HOME)/libsodium-install/lib
SODIUM_CFLAGS   := $(shell pkg-config --cflags libsodium) -I$(HOME)/libsodium-install/include

PROTOBUF_LDFLAGS  := $(shell pkg-config --libs protobuf)
PROTOBUF_CFLAGS   := $(shell pkg-config --cflags protobuf)
PROTOBUF_PATH     := $(BUILD_ROOT)/common/deps_/proto/
PROTOBUF_PROTOS   := $(wildcard $(BUILD_ROOT)/common/deps_/proto/*.proto)
PROTOBUF_HEADERS  := $(patsubst %.proto,%.pb.h,$(PROTOBUF_PROTOS))

COMMON_LDFLAGS  := $(BUILD_ROOT)/common/libcommon.a $(BUILD_ROOT)/common/deps_/proto/libproto.a $(BUILD_ROOT)/common/deps_/fsm/libfsm.a
COMMON_CFLAGS   := -I$(BUILD_ROOT)/common -I$(BUILD_ROOT)/common/deps_/proto -I$(BUILD_ROOT)/common/deps_/fsm/src

# FIXME on Windows
FIX_CXX_11_BUG :=
LINUX_LDFLAGS :=
ifeq ($(shell uname), Linux)
FIX_CXX_11_BUG :=  -Wl,--no-as-needed
LINUX_LDFLAGS :=  -pthread
SODIUM_LDFLAGS  += -lrt
endif

MAC_CFLAGS :=
ifeq ($(shell uname), Darwin)
MAC_CFLAGS := -DCOMMON_MAC_BUILD
endif

CFLAGS    += $(MAC_CFLAGS)
ifeq ($(RELEASE), 1)
CXXFLAGS  += -std=c++11 -fPIC $(FIX_CXX_11_BUG) $(LINUX_LDFLAGS) $(COMMON_CFLAGS) $(MAC_CFLAGS) $(PROTOBUF_CFLAGS) $(ZMQ_CFLAGS) $(SODIUM_CFLAGS) $(CPPFLAGS) -O3
LDFLAGS   += $(FIX_CXX_11_BUG) $(LINUX_LDFLAGS) $(COMMON_LDFLAGS) $(PROTOBUF_LDFLAGS) $(ZMQ_LDFLAGS) $(SODIUM_LDFLAGS) -O3
else
CXXFLAGS  += -std=c++11 -fPIC $(FIX_CXX_11_BUG) $(LINUX_LDFLAGS) $(COMMON_CFLAGS) $(MAC_CFLAGS) $(PROTOBUF_CFLAGS) $(ZMQ_CFLAGS) $(SODIUM_CFLAGS) $(CPPFLAGS) -g3
LDFLAGS   += $(FIX_CXX_11_BUG) $(LINUX_LDFLAGS) $(COMMON_LDFLAGS) $(PROTOBUF_LDFLAGS) $(ZMQ_LDFLAGS) $(SODIUM_LDFLAGS) -g3
endif
