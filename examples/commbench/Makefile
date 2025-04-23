# Compiler and flags
CXX := g++
CXXFLAGS := -g -std=c++11 -pthread -I ../..

OBJS=commbench.o pingpong.o

# Source files
SRCS := ../../convcore.C ../../scheduler.C ../../queue.C ../../conv-conds.C
HDRS := ../../converse_internal.h ../../scheduler.h ../../queue.h ../../barrier.h

# Default target
all: commbench

# Link the object files to create the executable
commbench: $(OBJS)
	$(CXX) -o commbench $(OBJS) $(CXXFLAGS)

commbench.o: commbench.C $(HDRS)
	$(CXX) -c commbench.C $(CXXFLAGS)

pingpong.o: pingpong.C $(HDRS)
	$(CXX) -c pingpong.C $(CXXFLAGS)

# Clean up build files
clean:
	rm -f *.o
	rm -f commbench

.PHONY: all clean
