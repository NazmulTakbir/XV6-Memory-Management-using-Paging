#include "types.h"
#include "user.h"

int main() {
    char* foo;
    for( int i=0; i<10; i++ ) {
        foo = malloc(4096);
        sleep(100);
    }
    foo[0] = foo[0];  // suppress compiler warning
    exit();
}