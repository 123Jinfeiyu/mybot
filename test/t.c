#include <stdio.h>
#include <pthread.h>
int main () {
	printf("pthread_t: %lu\n", sizeof(pthread_t));
	printf("int: %lu\n", sizeof(int));
}
