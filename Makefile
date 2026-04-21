VERSION := 0.0.2

APP_NAME := bot

CC:= gcc
CXX:= g++

LIBS := -lpthread

FLAGS := -O3 -pipe #-Wall -Werror

#-----------
all: libbot.so libfft.so
libbot.so:  src/cpp/I2C.c src/cpp/Matrix.cpp
	$(CC) $(FLAGS) -shared -fPIC $^ -o $@

libfft.so:  src/cpp/fft1.c src/cpp/fft.cpp
	$(CC) $(FLAGS) -shared -fPIC $^ -lm -o $@

clear:
	rm libbot.so
	rm libfft.so
