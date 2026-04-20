VERSION := 0.0.2

APP_NAME := bot

CC:= gcc
CXX:= g++

LIBS := -lpthread

FLAGS := -O3 -pipe -Wall -Werror

#-----------
all: libbot.so libfft.so test_fft1m1
libbot.so:  src/I2C.c src/Matrix.cpp
	$(CC) $(FLAGS) -shared -fPIC $^ -o $@

libfft.so:  src/fft1.c src/fft.cpp
	#$(CC) $(FLAGS) src/fft1.c -fPIC -c -o fft1.o
	#$(CXX) $(FLAGS) src/fft.cpp -fPIC -c -o fft.o
	$(CC) $(FLAGS) -shared -fPIC $^ -lm -o $@

test: test_fft1m1
test_fft1m1:  src/test_fft1m1.c
	$(CC) $(FLAGS) $^ -lm -o $@

clear:
	rm libbot.so
	rm libfft.so
	rm test_fft1m1
