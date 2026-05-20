#include <stdio.h>
#include <pthread.h>
int main () {
	printf("pthread_t: %lu\n", sizeof(pthread_t));
	printf("pthread_mutex_t: %lu\n", sizeof(pthread_mutex_t));
	printf("pthread_mutexattr_t: %lu\n", sizeof(pthread_mutexattr_t));
	printf("int: %lu\n", sizeof(int));
}
