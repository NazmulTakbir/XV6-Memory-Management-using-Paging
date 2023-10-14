#include "types.h"
#include "stat.h"
#include "user.h"
 
int main(int argc, char * argv[]) {
    uint totalPages=30;
    uint baseSize=(uint)sbrk(0), maxSize=totalPages*4096, currentSize;

    for( currentSize=baseSize; currentSize<=maxSize; currentSize+=4096 ) {
        sbrk(4096);
    }
    printf(1, "\n-----------------------------------------------------\n");
    printf(1, "Allocating Maximum Number of Pages Possible");
    printf(1, "\n-----------------------------------------------------\n");
    meminfo();

    for( ; currentSize>baseSize; currentSize-=4096 ) {
        sbrk(-4096);
    }
    printf(1, "\n-----------------------------------------------------\n");
    printf(1, "Deallocating All Pages except Code and Stack Pages");
    printf(1, "\n-----------------------------------------------------\n");
    meminfo();

	exit();
}