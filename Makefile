VERSION := 0.0.2

APP_NAME := bot

CC:= gcc

LIBS := -lpthread

FLAGS := -O3 -pipe -Wall -Werror

#-----------
libbot.so:  src/I2C.c src/PCA9685.c src/MPU6050.c
	$(CC) $(FLAGS) -shared -fPIC $^ -o $@
