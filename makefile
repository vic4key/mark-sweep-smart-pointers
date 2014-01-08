CXXFLAGS = -Wall -std=c++0x -pthread

all: test

test: test.o gcptr.o
	$(CXX) -o test test.o gcptr.o -lpthread

test.o: gcptr.h
gcptr.o: gcptr.h
