/* Host-only kernel shims. No ARM64 execution or hardware/VM claims. */
#ifndef NLH_TEST_SHIM_H
#define NLH_TEST_SHIM_H
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <openssl/sha.h>
#ifndef EPROGMISMATCH
#define EPROGMISMATCH 75
#endif
#define LCK_GRP_DECLARE(name, label) int name
#define LCK_MTX_DECLARE(name, group) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define LCK_SPIN_DECLARE(name, group) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define lck_mtx_lock pthread_mutex_lock
#define lck_mtx_unlock pthread_mutex_unlock
#define lck_spin_lock pthread_mutex_lock
#define lck_spin_unlock pthread_mutex_unlock
typedef uintptr_t thread_t;
typedef int wait_result_t;
typedef int boolean_t;
#define FALSE 0
#define TRUE 1
#define THREAD_NULL 0
#define KERN_SUCCESS 0
#define KHEAP_DEFAULT 0
#define Z_WAITOK 1
#define Z_ZERO 2
#define PZERO 0
static inline boolean_t ml_set_interrupts_enabled(boolean_t x) { (void)x; return TRUE; }
static inline thread_t current_thread(void) { return (thread_t)pthread_self(); }
static inline void thread_terminate(thread_t t) { (void)t; }
static inline void thread_deallocate(thread_t t) { (void)t; }
static void (*test_worker)(void *, wait_result_t);
static void *test_worker_arg;
static inline int kernel_thread_start(void (*f)(void *, wait_result_t), void *a, thread_t *t)
{ test_worker = f; test_worker_arg = a; *t = 1; return 0; }
static inline uint64_t mach_absolute_time(void)
{ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ULL + ts.tv_nsec; }
static inline void absolutetime_to_nanoseconds(uint64_t t, uint64_t *n) { *n = t; }
static inline void *kheap_alloc(int heap, size_t n, int flags)
{ (void)heap; (void)flags; return calloc(1, n); }
static inline void kheap_free(int heap, void *p, size_t n) { (void)heap; (void)n; free(p); }
static inline void *kalloc_data(size_t n, int flags) { (void)flags; return calloc(1, n); }
static inline void kfree_data(void *p, size_t n) { (void)n; free(p); }
static inline int msleep(void *a, void *lock, int pri, const char *name, struct timespec *ts)
{ (void)a; (void)lock; (void)pri; (void)name; return nanosleep(ts, NULL); }
#endif
