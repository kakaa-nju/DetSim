#include <stdio.h>

struct point {
    int x;
    int y;
};

struct line {
    struct point points[10];
    int count;
};

struct rect {
    struct point top_left;
    struct point bottom_right;
    struct line *border;
};

struct point g_point = {10, 20};
struct line g_line = {
    .points = {{0, 0}, {10, 10}, {20, 20}},
    .count = 3
};
struct rect g_rect = {
    .top_left = {0, 0},
    .bottom_right = {100, 100},
    .border = &g_line
};

int main() {
    printf("test\n");
    return 0;
}
