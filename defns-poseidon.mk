# Requires BOSEN_ROOT to be defined

BOSEN_SRC = $(BOSEN_ROOT)/src
BOSEN_LIB = $(BOSEN_ROOT)/lib
BOSEN_THIRD_PARTY = $(BOSEN_ROOT)/../third_party
BOSEN_THIRD_PARTY_SRC = $(BOSEN_THIRD_PARTY)/src
BOSEN_THIRD_PARTY_INCLUDE = $(BOSEN_THIRD_PARTY)/include
BOSEN_THIRD_PARTY_LIB = $(BOSEN_THIRD_PARTY)/lib
BOSEN_THIRD_PARTY_BIN = $(BOSEN_THIRD_PARTY)/bin

BOSEN_CXX = g++
BOSEN_CXXFLAGS = -g \
	   -O3 \
           -std=c++11 \
           -Wall \
	   -Wno-sign-compare \
           -fno-builtin-malloc \
           -fno-builtin-calloc \
           -fno-builtin-realloc \
           -fno-builtin-free \
           -fno-omit-frame-pointer
BOSEN_CXXFLAGS += -static-libstdc++

BOSEN_INCFLAGS = -I$(BOSEN_SRC) -I$(BOSEN_THIRD_PARTY_INCLUDE)
BOSEN_LDFLAGS_DIRS = -Wl,-rpath,$(BOSEN_THIRD_PARTY_LIB) \
          -L$(BOSEN_THIRD_PARTY_LIB) 
BOSEN_LDFLAGS_LIBS = -pthread -lrt -lnsl -luuid \
          -lzmq \
          -lglog \
          -lgflags \
          -ltcmalloc \
          -lconfig++ \
	  -lyaml-cpp \
	  -lleveldb \
          -lnuma
BOSEN_PS_LIB = $(BOSEN_LIB)/libpetuum-ps.a
BOSEN_PS_SN_LIB = $(BOSEN_LIB)/libpetuum-ps-sn.a
