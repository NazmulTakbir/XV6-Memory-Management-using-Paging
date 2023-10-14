#include "types.h"
#include "user.h"

int seed=13, a=7, c=67, m=255;

int rand() {
  seed = (a * seed + c) % m;
  return seed;
}

int main(int argc, char** argv) {
    int **str; 
    int pages = 10;

    verbose(2);

    if( argc==2 ) pages = atoi(argv[1]);

    int summation1 = 0;
    int summation2 = 0;

    str = malloc(sizeof(int*)*pages);
    for( int i=0; i<pages; i++ ) {
        str[i] = malloc(sizeof(int)*1024);
        for( int j=0; j<1024; j++ ) {
            int r = rand();
            str[i][j] = r;
            summation1 += str[i][j];
        }
    }

    for( int i=0; i<pages; i++ ) 
        for( int j=0; j<1024; j++ ) summation2 += str[i][j];

    printf(1, "Summation 1: %d,  Summation 2: %d\n", summation1, summation2);

    for( int i=0; i<pages; i++ ) free(str[i]);
    free(str);

    exit();
}