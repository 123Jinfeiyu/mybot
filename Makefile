MODE ?= cpu
VERSION := 0.0.2
APP_NAME := bot
CC:= gcc
CXX:= g++
NVCC:= nvcc
LIBS := -lpthread
FLAGS := -O3 -pipe -std=c++17 #-Wall -Werror

#-----------
all: libi2c4cj.so libmt4cj.so libfft4cj.so libcnn4cj.so

# 机器人及cpu神经网络、矩阵
libi2c4cj.so:  src/cpp/I2C.c
	$(CC) $(FLAGS) -shared -fPIC $^ -o $@

ifeq ($(MODE), cpu)
libmt4cj.so: src/cpp/Matrix.cpp
	$(CC) $(FLAGS) -shared -fPIC $^ -o $@
endif

ifeq ($(MODE), cuda)# GPU矩阵
libmt4cj.so: src/cuda/matrix.cu
	#$(NVCC) -arch=compute_61 -code=sm_61 -O3 -Xcompiler -fPIC -shared  src/cuda/matrix.cu -o libmt4cj.so
	$(NVCC) -fPIC -shared  src/cuda/matrix.cu -o libmt4cj.so

endif

# 快速傅里叶变换fft
libfft4cj.so:  src/cpp/fft1.c src/cpp/fft.cpp
	$(CC) $(FLAGS) -shared -fPIC $^ -lm -o $@

# 卷积神经网络cnn
libcnn4cj.so:  src/cpp/cnn/cnn_i.cpp conv.o pooling.o ga_pooling.o flatten.o imgl.o
	$(CC) $(FLAGS) -shared -fPIC $^ -lm -o $@

conv.o:  src/cpp/cnn/conv.cpp
	$(CC) $(FLAGS) -shared -fPIC -c $^ -o $@

pooling.o:  src/cpp/cnn/pooling.cpp
	$(CC) $(FLAGS) -shared -fPIC -c $^ -o $@

ga_pooling.o: src/cpp/cnn/ga_pooling.cpp
	$(CC) $(FLAGS) -shared -fPIC -c $^ -o $@

flatten.o:  src/cpp/cnn/flatten.cpp
	$(CC) $(FLAGS) -shared -fPIC -c $^ -o $@

imgl.o:  src/cpp/cnn/imgl.cpp
	$(CC) $(FLAGS) -shared -fPIC -c $^ -o $@


clear:
	rm *.o
	rm *.so
