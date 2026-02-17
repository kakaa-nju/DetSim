/* Comprehensive expression evaluation test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Basic types */
int g_int = 42;
long g_long = 123456789;
char g_char = 'A';
unsigned int g_uint = 0xDEADBEEF;

/* Arrays */
int g_int_arr[5] = {10, 20, 30, 40, 50};
char g_str[] = "Hello, World!";

/* Pointers - initialized to point to actual data */
int *g_int_ptr = &g_int;
char *g_str_ptr = g_str;

/* Struct with basic members */
struct basic {
    int x;
    int y;
    char c;
};
struct basic g_basic = {100, 200, 'Z'};
struct basic g_basic_arr[3] = {
    {1, 2, 'a'},
    {3, 4, 'b'},
    {5, 6, 'c'}
};

/* Nested struct */
struct inner {
    int value;
    char *name;
};
struct outer {
    struct inner in;
    int id;
};
struct inner g_inner = {999, "inner_struct"};
struct outer g_outer = {{888, "nested"}, 777};

/* Forward declarations for pointer initialization */
extern int g_int;
extern char g_str[];
extern struct basic g_basic;

/* Pointer to struct */
struct basic *g_basic_ptr = &g_basic;

/* Double pointer - initialized to point to g_int_ptr */
int **g_double_ptr = &g_int_ptr;

/* Linked list node */
struct node {
    int data;
    struct node *next;
};
struct node g_node1 = {1, NULL};
struct node g_node2 = {2, NULL};  /* Will be fixed in main */
struct node g_node3 = {3, NULL};  /* Will be fixed in main */
struct node *g_head = NULL;       /* Will be fixed in main */

/* Complex nested structure */
struct point {
    int x, y;
};
struct rect {
    struct point tl;
    struct point br;
    char *label;
};
struct rect g_rect = {{10, 20}, {100, 200}, "rectangle"};
struct rect *g_rect_ptr = &g_rect;

int main() {
    /* Setup linked list - nodes are defined in reverse order in C */
    /* We need to set up the chain: g_node1 -> g_node2 -> g_node3 */
    g_node1.next = &g_node2;
    g_node2.next = &g_node3;
    g_head = &g_node1;
    
    /* Setup double pointer - must be done after g_int_ptr is defined */
    g_double_ptr = &g_int_ptr;
    
    /* Trigger syscall for debugger */
    write(1, "expr_test\n", 10);
    
    return 0;
}

/* Test assertions:
 * tracee0:g_int == 42
 * tracee0:g_long == 123456789
 * tracee0:g_char == 'A'
 * tracee0:g_int_arr[2] == 30
 * tracee0:g_str[0] == 'H'
 * tracee0:*g_int_ptr == 42
 * tracee0:g_basic.x == 100
 * tracee0:g_basic.y == 200
 * tracee0:g_basic_arr[1].y == 4
 * tracee0:g_outer.in.value == 888
 * tracee0:g_outer.in.name[0] == 'n'
 * tracee0:g_basic_ptr->x == 100
 * tracee0:g_rect_ptr->br.x == 100
 * tracee0:g_head->next->data == 2
 * tracee0:g_head->next->next->data == 3
 * tracee0:**g_double_ptr == 42
 */
