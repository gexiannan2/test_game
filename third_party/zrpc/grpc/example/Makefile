CXX=g++
LDFLAGS= -lpthread -lprotobuf -lrt -lz -lstdc++fs 
CXXFLAGS= -Wall -w -g -ggdb -O2 -std=c++17 -Wno-unused -Wno-sign-compare -Wno-deprecated-declarations -Wno-deprecated -Wl,--no-as-needed -std=c++17 

.PHONY: clean all test

all: rpc_client rpc_server rpc_example_test

ifndef LIBNET_PATH
  $(warning Warning: missing zrpc path, using default)
	LIBNET_PATH=../../../../lib
endif
LIBNET_INCLUDE_DIR=$(LIBNET_PATH)
LIBNET_LIBRARY=$(LIBNET_PATH)/libzrpc.a

CXXFLAGS+= -I $(LIBNET_INCLUDE_DIR) -I ../../../ 

DEP_LIBS = $(LIBNET_LIBRARY)
LDFLAGS := $(DEP_LIBS) $(LDFLAGS)

sudoku.pb.cc: sudoku.proto
	protoc -I=. --cpp_out=. ./$^

rpc_client: client.cc sudoku.pb.cc
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

rpc_server: server.cc sudoku.pb.cc
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

rpc_example_test: example_test.cc sudoku.pb.cc
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

test: rpc_example_test
	./rpc_example_test

clean:
	find . -name "*.[oda]" -exec rm -f {} \;
	rm -rf rpc_client rpc_server rpc_example_test
