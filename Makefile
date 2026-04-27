MODE ?= cpu
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
libi2c4cj.so:  src/cpp/I2C.c
	$(CC) $(FLAGS) -shared -fPIC $^ -o $@

ifeq ($(MODE), cpu)
libmt4cj.so: src/cpp/Matrix.cpp
	$(CC) $(FLAGS) -shared -fPIC $^ -o $@
endif

ifeq ($(MODE), cuda)# GPU矩阵
libmt4cj.so: src/cuda/cu4cj.cu
	#$(NVCC) -arch=compute_61 -code=sm_61 -O3 -Xcompiler -fPIC -shared  src/cuda/cu4cj.cu -o libbot.so
	$(NVCC) -fPIC -shared  src/cuda/cu4cj.cu -o libmt4cj.so

endif

# 快速傅里叶变换fft
libfft4cj.so:  src/cpp/fft1.c src/cpp/fft.cpp
	$(CC) $(FLAGS) -shared -fPIC $^ -lm -o $@

clear:
	rm libi2c4cj.so
	rm libmt4cj.so
	rm libfft4cj.so