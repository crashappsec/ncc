// Broken once: applied to a variable declaration, not a function.
#include <stdio.h>

// Bug: _Once on a variable makes no sense
_Once int counter = 0;

int main(void) { printf("%d\n", counter); return 0; }
