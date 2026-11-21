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

/* Global variables for testing.
 *
 * Every value an assertion names is established by these initializers, so the
 * properties hold from exec onwards. Building them in main() instead would make
 * them unreadable at the first captured state (the program is stopped at
 * kernel-entry boundaries, and its first syscall can land mid-initialization --
 * the first malloc already produces states of its own). */
struct point g_point = {10, 20, 30};

struct line g_line = {
    .points = {
        [0] = {0, 0, 0},
        [1] = {10, 20, 30},
        [2] = {20, 40, 60},
    },
    .count = 3,
};

struct rect g_rect = {
    .top_left = {10, 20, 30},
    .bottom_right = {100, 200, 0},
    .border = &g_line,
};

struct node g_node = {
    .value = 42,
    .data = &g_rect,
};

/* A static table replaces the malloc'd one: allocation would put heap syscalls
 * between exec and the values above, and those states describe a program whose
 * initialization has not finished. */
static struct rect *g_rect_table[2] = {&g_rect, &g_rect};
static char g_container_name[] = "test_container";

struct container g_container = {
    .rects = g_rect_table,
    .num_rects = 2,
    .nodes = {
        [0] = {.value = 0},
        [1] = {.value = 100},
        [2] = {.value = 200},
        [3] = {.value = 300},
        [4] = {.value = 400},
    },
    .name = g_container_name,
};

struct tree_node g_tree_root = {
    .key = 1,
    .container = &g_container,
};

/* The linked list is published the same way; nodes are declared tail-first so
 * the static initializers can chain them. */
static struct node g_node3 = {.value = 3};
static struct node g_node2 = {.value = 2, .next = &g_node3};
static struct node g_node1 = {.value = 1, .next = &g_node2};
struct node *g_node_list = &g_node1;

int main() {
    /* Back links are the only part a static initializer cannot express, and no
     * assertion reads them. */
    g_node2.prev = &g_node1;
    g_node3.prev = &g_node2;

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
