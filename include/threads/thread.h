#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

#define FDT_PAGES 2
#define FDT_COUNT_LIMIT 128
/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */



struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* Priority. */
	int64_t awake_ticks;				/* awake ticks */
                                                                                                                                                                                                                                                                                                                                                                                                    
	/* Shared between thread.c and synch.c(semaphore->waiters). */
	struct list_elem elem;              /* List element. */

	int init_priority;
	struct lock *wait_on_lock;
	struct list donations;				
	struct list_elem donation_elem;

	int exit_status; //스레드 구조체 수정 -> _exit(), _wait()에 사용
	struct file **fdt;
	int next_fd;

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	// 자식 프로세스 생성시 지금은 fork를 수행하면서 context switch가 일어난 상태로
	// 현재 부모에는 커널이 작업하던 정보가 저장되어 있음
	// 따라서 syscall에서 f를 fork 함수에 전달해서 활용해야 한다.
	struct intr_frame parent_if;
	// 자식 지정 리스트를 설정하기 위한 필드 child_list, child_elem을 추가한다
	struct list child_list;
	struct list_elem child_elem;
	// 부모는 이 load가 완료될 때까지 대기해야함
	// semaphore를 활용해 Load가 완료될 때까지 부모를 재우자
	struct semaphore load_sema;
	struct semaphore exit_sema;
	struct semaphore wait_sema;

	struct file *running;
	unsigned magic;                     /* Detects stack overflow. */

};

struct thread *get_child_process(int pid);

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);


void test_max_priority (void);
bool cmp_priority_wait(const struct list_elem* A, const struct list_elem *B, void *aux);
bool cmp_priority_ready(const struct list_elem* A, const struct list_elem *B, void *aux);
void thread_awake(int64_t ticks);
void thread_wait();

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

struct list* thread_get_wait_list(void);			/* wait list의 주소 반환 */

int thread_get_priority (void);
void thread_set_priority (int);
bool
thread_compare_donate_priority (const struct list_elem *l, 
				const struct list_elem *s, void *aux UNUSED);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);
void donate_priority();

void remove_with_lock(struct lock *lock);
void refresh_priority (void);
bool check_priority_threads();
bool
thread_compare_donate_priority (const struct list_elem *l, 
				const struct list_elem *s, void *aux UNUSED);

#endif /* threads/thread.h */
