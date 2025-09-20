CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
TARGET = timestamp_calibration
SOURCE = timestamp_calibration.cpp

$(TARGET): $(SOURCE)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SOURCE)

clean:
	rm -f $(TARGET) timestamp_calibration.csv

.PHONY: clean
