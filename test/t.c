#include <stdio.h>
#include <pthread.h>
int main () {
	printf("%lu\n", sizeof(pthread_t));
}
