CXX=g++
LDFLAGS= -lpthread -lprotobuf -lrt -lz -lstdc++fs
CXXFLAGS= -Wall -w -g -ggdb -O2 -std=c++17 -Wno-unused -Wno-sign-compare -Wno-deprecated-declarations -Wno-deprecated -Wl,--no-as-needed

.PHONY: clean all test test-all

all: rpc_client rpc_server rpc_example_test direct_rpc_test

ifndef LIBNET_PATH
  $(warning Warning: missing zrpc path, using default)
	LIBNET_PATH=../../../../lib
endif
LIBNET_INCLUDE_DIR=$(LIBNET_PATH)
LIBNET_LIBRARY=$(LIBNET_PATH)/libzrpc.a

CXXFLAGS+= -I $(LIBNET_INCLUDE_DIR) -I ../../../

DEP_LIBS = $(LIBNET_LIBRARY)
LDFLAGS := $(DEP_LIBS) $(LDFLAGS)

GEN_ZRPC=python3 ../tools/gen_zrpc.py

echo.pb.cc: echo.proto
	protoc -I=. --cpp_out=. ./$^

echo.zrpc.h: echo.rpc ../tools/gen_zrpc.py
	$(GEN_ZRPC) echo.rpc -o $@

rpc_client: client.cc echo.pb.cc echo.zrpc.h
	$(CXX) $(CXXFLAGS) client.cc echo.pb.cc -o $@ $(LDFLAGS)

rpc_server: server.cc echo.pb.cc echo.zrpc.h
	$(CXX) $(CXXFLAGS) server.cc echo.pb.cc -o $@ $(LDFLAGS)

rpc_example_test: example_test.cc echo.pb.cc echo.zrpc.h
	$(CXX) $(CXXFLAGS) example_test.cc echo.pb.cc -o $@ $(LDFLAGS)

direct_rpc_test: direct_rpc_test.cc
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

test: rpc_example_test
	./rpc_example_test

test-all: rpc_example_test direct_rpc_test
	./rpc_example_test
	./direct_rpc_test

clean:
	find . -name "*.[oda]" -exec rm -f {} \;
	rm -rf rpc_client rpc_server rpc_example_test direct_rpc_test echo.pb.cc echo.pb.h echo.zrpc.h
