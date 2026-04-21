VERSION := 0.0.2

APP_NAME := bot

CC:= gcc
CXX:= g++
NVCC:= nvcc

LIBS := -lpthread

FLAGS := -O3 -pipe #-Wall -Werror

#-----------
all: libbot.so libfft.so

# 机器人及cpu神经网络、矩阵
libbot.so:  src/cpp/I2C.c src/cpp/Matrix.cpp
	$(CC) $(FLAGS) -shared -fPIC $^ -o $@

# 快速傅里叶变换fft
libfft.so:  src/cpp/fft1.c src/cpp/fft.cpp
	$(CC) $(FLAGS) -shared -fPIC $^ -lm -o $@

cuda: libcu.so

# GPU矩阵
libcu.so: src/cuda/Matrix.cu
	$(NVCC) -arch=compute_61 -code=sm_61 -O3 -Xcompiler -fPIC \
	 -shared  src/cuda/Matrix.cu -o libcu.so

clear:
	rm libbot.so
	rm libfft.so
	rm libcu.so