all: rswap-server

RSWAP_ROOT_PATH := $(shell dirname $(abspath $(lastword $(MAKEFILE_LIST))))

OFA_DIR ?= /usr/src/ofa_kernel/default
OFA_INCLUDE := -I$(OFA_DIR)/include

INCLUDES = -I$(RSWAP_ROOT_PATH)/..
INCLUDES += $(OFA_INCLUDE)
LIBS = -pthread -lrdmacm -libverbs

CXXFLAGS =-std=c++11 -Wall -Werror -O2
rswap-server: rswap_server.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(LIBS)

run:
	./rswap-server

clean:
	rm rswap-server
