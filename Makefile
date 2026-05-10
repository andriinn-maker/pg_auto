# Makefile для PostgreSQL Tuner (macOS, C++17)
# Использование: make          - собрать pg_tuner
#               make clean     - удалить объектные файлы и исполняемый
#               make install   - скопировать в /usr/local/bin (требует sudo)

CXX = clang++
CXXFLAGS = -std=c++17 -Wall -O2
LDFLAGS =
TARGET = pg_auto
SRC = pg_auto.cpp
OBJ = $(SRC:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)

.PHONY: all clean install
