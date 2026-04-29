nvcc -shared -o libmt4cj.dll src/cuda/matrix.cu -allow-unsupported-compiler
#nvcc -shared -o libmt4cj.dll src/cpp/Matrix.cpp -allow-unsupported-compiler
nvcc -shared -o libi2c4cj.dll src/cpp/I2C.c -allow-unsupported-compiler
nvcc -shared -o libfft4cj.dll src/cpp/fft.cpp src/cpp/Matrix.cpp src/cpp/fft1.c -allow-unsupported-compiler
nvcc -shared -o libconv4cj.dll src/cpp/cnn/conv.cpp -allow-unsupported-compiler
nvcc -shared -o libimgl4cj.dll src/cpp/cnn/imgl.cpp -allow-unsupported-compiler
nvcc -shared -o libpooling4cj.dll src/cpp/cnn/pooling.cpp -allow-unsupported-compiler
nvcc -shared -o libga_pooling4cj.dll src/cpp/cnn/ga_pooling.cpp -allow-unsupported-compiler
nvcc -shared -o libflatten4cj.dll src/cpp/cnn/flatten.cpp -allow-unsupported-compiler
