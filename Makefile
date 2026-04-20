VERSION := 0.0.2

APP_NAME := bot

CC:= gcc
CXX:= g++

LIBS := -lpthread

FLAGS := -O3 -pipe #-Wall -Werror

#-----------
all: libbot.so libfft.so
libbot.so:  src/I2C.c src/Matrix.cpp
	$(CC) $(FLAGS) -shared -fPIC $^ -o $@

libfft.so:  src/fft1.c src/fft.cpp
	$(CC) $(FLAGS) -shared -fPIC $^ -lm -o $@

clear:
	rm libbot.so
	rm libfft.so
