#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Forward declaration */
struct node;

/* Simple struct with array */
struct point {
    int x;
    int y;
    int z;
};

/* Struct containing array */
struct line {
    struct point points[10];
    int count;
};

/* Nested struct */
struct rect {
    struct point top_left;
    struct point bottom_right;
    struct line *border;
};

/* Self-referential struct (linked list node) */
struct node {
    int value;
    struct node *next;
    struct node *prev;
    struct rect *data;
};

/* Struct with pointer to array */
struct container {
    struct rect **rects;      /* Array of rect pointers */
    int num_rects;
    struct node nodes[5];     /* Array of nodes */
    char *name;
};

/* Complex nested struct */
struct tree_node {
    int key;
    struct tree_node *left;
    struct tree_node *right;
    struct tree_node *parent;
    struct container *container;
};

/* Global variables for testing */
struct point g_point = {10, 20, 30};
struct line g_line;
struct rect g_rect;
struct node g_node;
struct container g_container;
struct tree_node g_tree_root;
struct node *g_node_list = NULL;

int main() {
    /* Initialize g_line */
    g_line.count = 3;
    for (int i = 0; i < 3; i++) {
        g_line.points[i].x = i * 10;
        g_line.points[i].y = i * 20;
        g_line.points[i].z = i * 30;
    }
    
    /* Initialize g_rect */
    g_rect.top_left = g_point;
    g_rect.bottom_right.x = 100;
    g_rect.bottom_right.y = 200;
    g_rect.bottom_right.z = 0;
    g_rect.border = &g_line;
    
    /* Initialize g_node */
    g_node.value = 42;
    g_node.next = NULL;
    g_node.prev = NULL;
    g_node.data = &g_rect;
    
    /* Initialize g_container */
    g_container.num_rects = 2;
    g_container.rects = malloc(sizeof(struct rect *) * 2);
    g_container.rects[0] = &g_rect;
    g_container.rects[1] = &g_rect;
    g_container.name = "test_container";
    for (int i = 0; i < 5; i++) {
        g_container.nodes[i].value = i * 100;
        g_container.nodes[i].next = (i < 4) ? &g_container.nodes[i+1] : NULL;
        g_container.nodes[i].prev = (i > 0) ? &g_container.nodes[i-1] : NULL;
    }
    
    /* Initialize g_tree_root */
    g_tree_root.key = 1;
    g_tree_root.left = NULL;
    g_tree_root.right = NULL;
    g_tree_root.parent = NULL;
    g_tree_root.container = &g_container;
    
    /* Create a linked list */
    struct node n1, n2, n3;
    n1.value = 1; n1.next = &n2; n1.prev = NULL;
    n2.value = 2; n2.next = &n3; n2.prev = &n1;
    n3.value = 3; n3.next = NULL; n3.prev = &n2;
    g_node_list = &n1;
    
    /* Trigger syscall for debugger */
    write(1, "complex test\n", 12);
    
    return 0;
}

/* Assertions for testing:
 * tracee0:g_point.x == 10
 * tracee0:g_line.points[2].y == 40
 * tracee0:g_rect.border->count == 3
 * tracee0:g_node.data->top_left.z == 30
 * tracee0:g_container.nodes[2].value == 200
 * tracee0:g_container.rects[0]->bottom_right.x == 100
 * tracee0:g_tree_root.container->name[0] == 't'
 * tracee0:g_node_list->next->value == 2
 */
