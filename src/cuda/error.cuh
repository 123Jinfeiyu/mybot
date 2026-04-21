#pragma once
#include <cuda_runtime.h>
#include <stdio.h>

#define CHECK(call) do {\
    cudaError_t err = call;\
    if (err != cudaSuccess) {\
        fprintf(stderr, "CUDA error at %s:%d\n", __FILE__, __LINE__);\
        fprintf(stderr, "%s\n", cudaGetErrorString(err));\
        exit(EXIT_FAILURE);\
    }\
} while (0)
