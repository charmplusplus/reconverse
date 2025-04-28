# Compiler and flags
CXX := g++
CONVERSE_SRC := ../../..
CXXFLAGS := -std=c++11 -pthread -I $(CONVERSE_SRC)

# Source files
COMMON_SRCS := $(CONVERSE_SRC)/convcore.cpp $(CONVERSE_SRC)/scheduler.cpp $(CONVERSE_SRC)/queue.cpp $(CONVERSE_SRC)/conv-conds.cpp
HDRS := $(CONVERSE_SRC)/converse_internal.h $(CONVERSE_SRC)/scheduler.h $(CONVERSE_SRC)/queue.h $(CONVERSE_SRC)/barrier.h

# Default target
all: pingpong pingpong_multipairs

# Link the object files to create the executable
pingpong: pingpong.cpp $(COMMON_SRCS)
	$(CXX) -o $@ $^ $(CXXFLAGS)

pingpong_multipairs: pingpong_multipairs.cpp $(COMMON_SRCS)
	$(CXX) -o $@ $^ $(CXXFLAGS)

testping: pingpong
	./pingpong +p 2 100 2 128 2   

testmulti: pingpong_multipairs
	./pingpong +p 2 100 2 128 2   

# Clean up build files
clean:
	rm -f $(TARGET)

.PHONY: all clean
