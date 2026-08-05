CXX ?= g++
BUILD_DIR ?= build
CPPFLAGS := -I. -I../build/include
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic \
	-Wno-unused-parameter -Wno-deprecated-declarations
LDLIBS := -lpthread -lprotobuf -lrt -lz

.PHONY: clean all

all: $(BUILD_DIR)/echo_client $(BUILD_DIR)/echo_server \
	$(BUILD_DIR)/event_loop_demo $(BUILD_DIR)/event_loop_stress \
	$(BUILD_DIR)/event_loop_engine_demo

LIBNET_LIBRARY ?= ../build/libzrpc.a

echo.pb.cc: echo.proto
	protoc -I=. --cpp_out=. ./$^

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/echo_client: echo_client.cc echo.pb.cc | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ $(LIBNET_LIBRARY) $(LDLIBS)

$(BUILD_DIR)/echo_server: echo_server.cc echo.pb.cc | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ $(LIBNET_LIBRARY) $(LDLIBS)

$(BUILD_DIR)/event_loop_demo: event_loop_demo.cc | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ $(LIBNET_LIBRARY) $(LDLIBS)

$(BUILD_DIR)/event_loop_stress: event_loop_stress.cc | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ $(LIBNET_LIBRARY) $(LDLIBS)

$(BUILD_DIR)/event_loop_engine_demo: event_loop_engine_demo.cc | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ $(LIBNET_LIBRARY) $(LDLIBS)

clean:
	rm -rf $(BUILD_DIR)
