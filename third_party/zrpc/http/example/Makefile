CXX=g++
LDFLAGS= -lpthread -lstdc++fs
CXXFLAGS= -Wall -w -g -ggdb -O0 -std=c++17 -Wno-unused -Wno-sign-compare -Wno-deprecated-declarations -Wno-deprecated -std=c++17

.PHONY: clean all test

all: echo_server auth_server auth_test

ifndef LIBNET_PATH
  $(warning Warning: missing zrpc path, using default)
	LIBNET_PATH=../../../../lib
endif
LIBNET_INCLUDE_DIR=$(LIBNET_PATH)
LIBNET_LIBRARY=$(LIBNET_PATH)/libzrpc.a

CXXFLAGS+= -I $(LIBNET_INCLUDE_DIR) -I ../../../

SSL_HAVE := $(shell pkg-config --exists openssl 2>/dev/null && echo yes || echo no)
ifeq ($(SSL_HAVE),no)
SSL_HAVE := $(shell test -f /usr/include/openssl/ssl.h && echo yes || echo no)
endif
ifeq ($(SSL_HAVE),yes)
CXXFLAGS += -DZRPC_USE_OPENSSL
SSL_LIBS := $(shell pkg-config --exists openssl 2>/dev/null && pkg-config --libs openssl || echo -lssl -lcrypto)
else
SSL_LIBS :=
endif

DEP_LIBS = $(LIBNET_LIBRARY)
LDFLAGS := $(DEP_LIBS) $(SSL_LIBS) $(LDFLAGS)

echo_server: echo_server.cc
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

auth_server: auth_server.cc
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

auth_test: auth_test.cc
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

test: auth_test
	./auth_test

clean:
	find . -name "*.[oda]" -exec rm -f {} \;
	rm -rf echo_server auth_server auth_test
