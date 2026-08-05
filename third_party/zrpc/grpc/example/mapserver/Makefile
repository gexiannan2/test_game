CXX = g++
AR = ar

ROOT := $(abspath ../../../../..)
ZRPC_DIR := $(ROOT)/src/zrpc
LIBZRPC := $(ROOT)/lib/libzrpc.a
JOLT_DIR := $(ROOT)/third_party/JoltPhysics
JOLT_BUILD := $(JOLT_DIR)/Build/Linux_Release
JOLT_LIB := $(JOLT_BUILD)/libJolt.a

CXXFLAGS = -Wall -w -g -ggdb -O2 -std=c++17 -Wno-unused -Wno-sign-compare \
           -Wno-deprecated-declarations -Wno-deprecated -DNDEBUG \
           -I . -I $(ROOT)/src -I $(ZRPC_DIR) -I $(JOLT_DIR)
LDFLAGS = -lpthread -lprotobuf -lrt -lz -lstdc++fs

SHARED_SRCS = shared/physics_world.cc shared/map_world.cc shared/proto_conv.cc
SHARED_OBJS = $(SHARED_SRCS:.cc=.o)

.PHONY: all clean jolt libzrpc proto

all: mapserver mapclient

libzrpc:
	$(MAKE) -C $(ZRPC_DIR)

jolt:
	@mkdir -p $(JOLT_BUILD)
	@if [ ! -f $(JOLT_LIB) ]; then \
		cd $(JOLT_BUILD) && cmake -DCMAKE_BUILD_TYPE=Release \
			-DJPH_BUILD_SHARED_LIBS=OFF \
			-DENABLE_ALL_WARNINGS=OFF \
			-DPROFILER_IN_DEBUG_AND_RELEASE=OFF \
			-DDEBUG_RENDERER_IN_DEBUG_AND_RELEASE=OFF \
			.. && $(MAKE) Jolt -j$$(nproc); \
	fi

mapserver.pb.cc mapserver.pb.h: mapserver.proto
	protoc -I=. --cpp_out=. ./mapserver.proto

proto: mapserver.pb.cc

%.o: %.cc mapserver.pb.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

mapserver: server.cc $(SHARED_OBJS) mapserver.pb.cc libzrpc jolt
	$(CXX) $(CXXFLAGS) server.cc $(SHARED_OBJS) mapserver.pb.cc -o $@ \
		$(LIBZRPC) $(JOLT_LIB) $(LDFLAGS)

mapclient: client.cc $(SHARED_OBJS) mapserver.pb.cc libzrpc jolt
	$(CXX) $(CXXFLAGS) client.cc $(SHARED_OBJS) mapserver.pb.cc -o $@ \
		$(LIBZRPC) $(JOLT_LIB) $(LDFLAGS)

clean:
	rm -f $(SHARED_OBJS) mapserver.pb.cc mapserver.pb.h mapserver mapclient
