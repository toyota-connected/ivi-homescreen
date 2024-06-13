#include <stdio.h>

int
main(int argc, char *argv[])
{
#ifdef NDEBUG
	fprintf(stderr, "NDEBUG is defined, cannot run tests\n");
	return 1;
#else
	return 0;
#endif
}
