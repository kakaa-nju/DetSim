/* cast_test.c - Test for type casting in expression evaluation */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A structure that will be hidden behind void* */
struct hidden_struct {
    int secret_value;
    char secret_name[32];
};

/* Container that hides the struct behind void* */
struct container {
    void *hidden_data;  /* Actually points to struct hidden_struct */
    int visible_count;
};

/* Global instances */
struct hidden_struct g_secret = {
    .secret_value = 42,
    .secret_name = "Hello World"
};

struct container g_container = {
    .hidden_data = &g_secret,  /* Hide the struct behind void* */
    .visible_count = 100
};

/* Another test for pointer to different struct types */
struct point {
    int x;
    int y;
};

struct point g_point = { 10, 20 };
void *g_generic_ptr = &g_point;

int main() {
    printf("Test for type casting\n");
    fflush(stdout);
    return 0;
}

/* Test assertions:
 * 
 * 1. Cast void* to struct hidden_struct*:
 *    ((struct hidden_struct*)g_container.hidden_data)->secret_value == 42
 * 
 * 2. Access the hidden struct's string member:
 *    ((struct hidden_struct*)g_container.hidden_data)->secret_name[0] == 'H'
 * 
 * 3. Cast void* to struct point*:
 *    ((struct point*)g_generic_ptr)->x == 10
 * 
 * 4. Cast struct pointer to void* (round-trip):
 *    (void*)&g_point == g_generic_ptr
 */
