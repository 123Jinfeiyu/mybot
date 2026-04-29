nvcc -shared -o libmt4cj.dll src/cuda/matrix.cu -allow-unsupported-compiler
#nvcc -shared -o libmt4cj.dll src/cpp/Matrix.cpp -allow-unsupported-compiler
nvcc -shared -o libi2c4cj.dll src/cpp/I2C.c -allow-unsupported-compiler
nvcc -shared -o libfft4cj.dll src/cpp/fft.cpp src/cpp/Matrix.cpp src/cpp/fft1.c -allow-unsupported-compiler
nvcc -shared -o conv.lib -c src/cpp/cnn/conv.cpp -allow-unsupported-compiler
nvcc -shared -o imgl.lib -c src/cpp/cnn/imgl.cpp -allow-unsupported-compiler
nvcc -shared -o pooling.lib -c src/cpp/cnn/pooling.cpp -allow-unsupported-compiler
nvcc -shared -o ga_pooling.lib -c src/cpp/cnn/ga_pooling.cpp -allow-unsupported-compiler
nvcc -shared -o flatten.lib -c src/cpp/cnn/flatten.cpp -allow-unsupported-compiler
nvcc -shared --std=c++17 -o libcnn4cj.dll src/cpp/cnn/cnn_i.cpp conv.lib imgl.lib pooling.lib ga_pooling.lib flatten.lib -allow-unsupported-compiler
