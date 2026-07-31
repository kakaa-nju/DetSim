/*
 * parentheses.c - Multi-threaded parentheses matching test for DetSim
 *
 * This program creates two threads:
 * - Thread 1: Writes left parentheses '('
 * - Thread 2: Writes right parentheses ')'
 *
 * An atomic counter tracks the nesting depth. The rules are:
 * - Left parenthesis can always be written (depth increases)
 * - Right parenthesis can only be written if depth > 0 (depth decreases)
 *
 * Using futex for synchronization to test DetSim's futex emulation.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <errno.h>

// Output buffer
#define OUTPUT_SIZE 1000
static char output[OUTPUT_SIZE];
static volatile int output_pos = 0;

// Parentheses depth counter (protected by futex)
static volatile int depth = 0;
static volatile int depth_futex = 0;

// Termination flag
static volatile int should_stop = 0;

// Futex wrapper functions
static int futex_wait(int *uaddr, int val) {
    return syscall(SYS_futex, uaddr, FUTEX_WAIT, val, NULL, NULL, 0);
}

static int futex_wake(int *uaddr, int nr) {
    return syscall(SYS_futex, uaddr, FUTEX_WAKE, nr, NULL, NULL, 0);
}

// Thread IDs for debugging
static volatile int left_tid = 0;
static volatile int right_tid = 0;

// Left parenthesis thread
static void *left_thread(void *arg) {
    left_tid = syscall(SYS_gettid);
    (void)arg;

    while (!should_stop && output_pos < OUTPUT_SIZE - 1) {
        // Left paren can always be written

        // Write to output
        int pos = __sync_fetch_and_add(&output_pos, 1);
        if (pos < OUTPUT_SIZE - 1) {
            output[pos] = '(';
        }

        // Increment depth
        int old_depth = __sync_fetch_and_add(&depth, 1);

        // If depth was 0, wake up right thread (it might be waiting)
        if (old_depth == 0) {
            futex_wake((int*)&depth_futex, 1);
        }

        // Small yield to allow interleaving
        syscall(SYS_sched_yield);
    }

    return NULL;
}

// Right parenthesis thread
static void *right_thread(void *arg) {
    right_tid = syscall(SYS_gettid);
    (void)arg;

    int starvation_counter = 0;

    while (!should_stop && output_pos < OUTPUT_SIZE - 1) {
        // Check if we can write right paren (depth must be > 0)
        int current_depth = depth;

        if (current_depth > 0) {
            // Try to decrement depth atomically
            if (__sync_bool_compare_and_swap(&depth, current_depth, current_depth - 1)) {
                // Success - write right paren
                int pos = __sync_fetch_and_add(&output_pos, 1);
                if (pos < OUTPUT_SIZE - 1) {
                    output[pos] = ')';
                }
                starvation_counter = 0;
            }
        } else {
            // Can't write - would make depth negative
            // Wait on futex (but with timeout to prevent permanent stall)
            starvation_counter++;

            if (starvation_counter > 100) {
                // Anti-starvation: force a yield and retry
                syscall(SYS_sched_yield);
                starvation_counter = 0;
            } else {
                // Wait for depth to become positive
                futex_wait((int*)&depth_futex, 0);
            }
        }
    }

    return NULL;
}

// Validate the output buffer
static int validate_output(void) {
    int current_depth = 0;
    int errors = 0;

    for (int i = 0; i < output_pos && i < OUTPUT_SIZE - 1; i++) {
        if (output[i] == '(') {
            current_depth++;
        } else if (output[i] == ')') {
            current_depth--;
            if (current_depth < 0) {
                fprintf(stderr, "ERROR: Unmatched ')' at position %d\n", i);
                errors++;
                current_depth = 0;  // Reset to continue checking
            }
        }
    }

    return errors;
}

// Print statistics
static void print_stats(void) {
    int left_count = 0, right_count = 0;

    for (int i = 0; i < output_pos && i < OUTPUT_SIZE - 1; i++) {
        if (output[i] == '(') left_count++;
        else if (output[i] == ')') right_count++;
    }

    printf("\n=== Statistics ===\n");
    printf("Total characters: %d\n", output_pos);
    printf("Left parens: %d\n", left_count);
    printf("Right parens: %d\n", right_count);
    printf("Final depth: %d\n", depth);
    printf("Left TID: %d, Right TID: %d\n", left_tid, right_tid);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("Parentheses matching test starting...\n");
    printf("Left thread writes '(', Right thread writes ')'\n");
    printf("Target output size: %d characters\n", OUTPUT_SIZE - 1);

    // Initialize output
    memset(output, 0, sizeof(output));

    // Create threads
    pthread_t left, right;

    if (pthread_create(&left, NULL, left_thread, NULL) != 0) {
        perror("pthread_create left");
        return 1;
    }

    if (pthread_create(&right, NULL, right_thread, NULL) != 0) {
        perror("pthread_create right");
        return 1;
    }

    // Wait for output to fill or timeout
    sleep(2);

    // Signal stop
    should_stop = 1;

    // Wake up any waiting threads
    futex_wake((int*)&depth_futex, 100);

    // Join threads
    pthread_join(left, NULL);
    pthread_join(right, NULL);

    // Null terminate output
    output[OUTPUT_SIZE - 1] = '\0';

    // Print results
    printf("\n=== Output (first 100 chars) ===\n");
    for (int i = 0; i < 100 && i < output_pos; i++) {
        putchar(output[i]);
    }
    printf("\n");

    if (output_pos > 100) {
        printf("... (%d more chars)\n", output_pos - 100);
    }

    print_stats();

    // Validate
    int errors = validate_output();
    if (errors == 0) {
        printf("\nSUCCESS: Output is a valid parentheses prefix!\n");
        return 0;
    } else {
        printf("\nFAILURE: %d validation errors found!\n", errors);
        return 1;
    }
}
