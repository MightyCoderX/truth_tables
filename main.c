#include <stdio.h>
#include <stdlib.h>

#include <readline/readline.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

typedef int (*bool_func_t)(int a, int b);

int main(void) {
    // TODO: Take input names directly instead of count
    int input_count = 0;
    printf("input count: ");
    int res = scanf("%d", &input_count);
    if(res == EOF) {
        perror("scanf");
        return 1;
    }

    char* inputs = malloc(input_count);

    for(int i = 0; i < input_count; i++) {
        printf("input #%d name: ", i);
        scanf(" %c", &inputs[i]);
    }

    int function_count = 0;
    printf("function count: ");
    res = scanf("%d", &function_count);
    if(res == EOF) {
        perror("scanf");
        return 1;
    }

    char** func_names = malloc(function_count * sizeof(*func_names));

    FILE* funcs_file = fopen("funcs.c", "w");

    for(int i = 0; i < function_count; i++) {
        printf("func #%d expression", i);
        char* expr = readline("> ");
        char* func = malloc(strlen(expr) + 1);

        sprintf(func, "f%d", i);

        func_names[i] = func;

        fprintf(funcs_file, "int %s(", func);
        for(int j = 0; j < input_count; j++) {
            fprintf(funcs_file, "int %c", inputs[j]);
            if(j + 1 < input_count) fprintf(funcs_file, ", ");
        }
        fprintf(funcs_file, ") { return %s; }\n", expr);
    }
    fclose(funcs_file);

    // Compile .c file with functions to .so
    pid_t child = fork();
    if(child == -1) {
        perror("fork");
        return 1;
    }
    if(child == 0) {
        int err = execlp("gcc", "gcc", "-Wall", "-o", "funcs.so", "funcs.c", "-shared", NULL);
        if(err == -1) {
            perror("execlp");
        }
        return 1;
    }
    int err = waitpid(child, NULL, 0);
    printf("after compile\n");
    if(err == -1) {
        perror("wait");
        return 1;
    }

    // Open file with dlopen
    void* handle = dlopen("./funcs.so", RTLD_NOW);
    if(handle == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    bool_func_t* funcs = malloc(function_count * sizeof(*funcs));

    for(int i = 0; i < function_count; i++) {
        printf("%s\n", func_names[i]);
        funcs[i] = dlsym(handle, func_names[i]);
        if(*funcs[i] == NULL) {
            fprintf(stderr, "error when getting function %s: %s\n", func_names[i], dlerror());
            return 1;
        }

        // TODO: Run funcs with all combinations of input values (1, 0)
        printf("funcs[i]=%p  *funcs[i]=%p\n", (void*)funcs[i], (void*)*funcs[i]);
        printf("%d\n", (funcs[i])(0, 1));
    }

    return 0;
}
