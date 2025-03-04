# Compiler and flags
CXX := g++
CXXFLAGS := -g -std=c++11 -pthread -I ../..

# Source files
SRCS := idle.C ../../convcore.C ../../scheduler.C ../../queue.C ../../conv-conds.C
HDRS := ../../convcore.h ../../scheduler.h ../../queue.h ../../barrier.h

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
