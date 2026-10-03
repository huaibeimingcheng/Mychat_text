CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -pthread
LDFLAGS  ?= -pthread

SRCS := $(wildcard common/*.cpp net/*.cpp app/*.cpp) main.cpp
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)
TARGET := chat_server

.PHONY: all clean run test dirs

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

run: $(TARGET) dirs
	./$(TARGET)

dirs:
	@mkdir -p data logs

test: $(TARGET)
	python3 tests/test_chat_server.py --start-server

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)
