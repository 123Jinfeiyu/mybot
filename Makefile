VERSION := 0.0.2

APP_NAME := bot

CC:= gcc

LIBS := -lpthread

FLAGS := -O3 -pipe -Wall -Werror

#-----------
libbot.so:  src/I2C.c src/Matrix.cpp
	$(CC) $(FLAGS) -shared -fPIC $^ -o $@

clear:
	rm libbot.so
