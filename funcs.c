#include <stddef.h>

int f0(int* inputs, size_t len) {
    int a = inputs[0];
    int b = inputs[1];
    return a && b;
}
int f1(int* inputs, size_t len) {
    int a = inputs[0];
    int b = inputs[1];
    return (a && !b) || (!a && b);
}
