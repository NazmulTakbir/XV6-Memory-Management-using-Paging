#include "types.h"
#include "stat.h"
#include "user.h"
 
int main(int argc, char * argv[]){
    verbose(0);
    int totalNumbers = 17*1024;

    int* addr = (int*) malloc(sizeof(int)*totalNumbers);
    for( int i=0; i<totalNumbers; i++ ) addr[i] = i*i;

    printf(1, "\nParent Process Memory:\n");
    meminfo();
    printf(1, "\n-----------------------------------------------------\n");
    printf(1, "Child Process Created");
    printf(1, "\n-----------------------------------------------------\n");

    int pid = fork();
    if( pid==0 ) {
        printf(1, "\nChild Process Memory:\n");
        meminfo();
    }
    
    int success = 1;

    for( int i=0; i<totalNumbers; i++ ) {
        if( addr[i] != i*i ) success = 0;
    }

    if( pid==0 ) {
        if(success) printf(1, "\nChild Test Successful, PID: %d\n", getpid());
        else printf(1, "\nChild Test Failed, PID: %d\n\n", getpid());
    }
    else {
        if(success) printf(1, "\nParent Test Successful, PID: %d\n", getpid());
        else printf(1, "\nParent Test Failed, PID: %d\n", getpid());
    }

    free((void*)addr);
    if( pid!=0 ) {
        wait();
        printf(1, "\n");
    }

	exit();
}