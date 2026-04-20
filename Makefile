VERSION := 0.0.2

APP_NAME := bot

CC:= gcc

LIBS := -lpthread

FLAGS := -O3 -pipe -Wall -Werror

#-----------
all: libbot.so test_fft
libbot.so:  src/I2C.c src/Matrix.cpp
	$(CC) $(FLAGS) -shared -fPIC $^ -o $@

test_fft:  src/test_fft1m1.c
	$(CC) $(FLAGS) -lm  $^ -o $@

clear:
	rm libbot.so
	rm test_fft
