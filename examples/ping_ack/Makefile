# Compiler and flags
CXX := g++
CXXFLAGS := -std=c++11 -pthread -I ../.. -g

# Source files
SRCS := ping.c ../../convcore.C ../../scheduler.C ../../queue.C ../../conv-conds.C
HDRS := ../../converse_internal.h ../../scheduler.h ../../queue.h ../../barrier.h

# Output executable
TARGET := test

# Default target
all: $(TARGET)

# Link the object files to create the executable
$(TARGET): $(SRCS)
	$(CXX) -o $@ $^ $(CXXFLAGS)

# Clean up build files
clean:
	rm -f $(TARGET)

.PHONY: all clean
