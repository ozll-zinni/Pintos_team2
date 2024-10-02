#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

/* TIMER_FREQ가 적절한 범위(19 이상 1000 이하) 내에 있는지 확인하는 코드
		* 동작 주파수가 너무 낮거나 높지 않도록 조절해줌
 */
#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
/* timer interrupt, 타이머 인터럽트가 발생하는 시간 간격 */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

//static struct thread* awake_thread;					/* wait_list에서 가장 먼저 깨울 스레드를 전역 변수로 관리 */

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void
timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
/* 타이머의 성능을 보정하기 위한 함수 */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
/* 운영체제가 부팅된 이후부터 timer ticks를 반환 */
int64_t
timer_ticks (void) {
	/* why? disable과 intr_set_level을 해주고, 그 다음에는 barrier()를 하는가? */
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/* Suspends execution for approximately TICKS timer ticks. */
void
timer_sleep (int64_t ticks) {
	int64_t start = timer_ticks ();

	ASSERT (intr_get_level () == INTR_ON);
	int64_t awake_ticks = start + ticks;

	// 현재 스레드의 awake_ticks를 넣어주고, wait_list에 넣어주고, block 처리(스레드 상태 변경)를 해준다.
	struct thread* curr = thread_current(); // running_thread 랑 고민됨..
	curr->awake_ticks = awake_ticks;
	thread_wait();

	// pooling 방식을 interrupt 방식으로 변경함에 따라 주석 처리
	// while (timer_elapsed (start) < ticks)
	// 	thread_yield ();
}

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;
	thread_awake(ticks);
	thread_tick ();
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) {
	/* Wait for a timer tick. */
	int64_t start = ticks;					/* start: 하나의 timer tick이 일어났는지 확인하기 위한 기준 tick 값 */
	while (ticks == start)
		barrier ();										/* 하나의 timer tick이 일어나기 전까지 메모리 읽기, 쓰기 순서를 보장함 */

	/* 루프가 하나의 timer tick보다 오래 기다림 */
	/* Run LOOPS loops. */
	start = ticks;									/* satrt: busy_wait()에서 timer_tick이 1 이상 증가했는지를 확인하기 위한 기준 tick 값 */
	busy_wait (loops);							/* loops만큼 루프를 돌면서 CPU 자원을 사용하면서 대기하는 역할 */

	/* If the tick count changed, we iterated too long. */
	barrier ();											/* timer_tick 값이 정확히 busy_wait에서 세팅된 timer_tick 값인지 확인 */
	return start != ticks;					/* busy_wait()가 하나의 timer tick이 일어나기 전에 완료되었는지 판단 */
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;								/* 대기하길 원하는 시간 / 환산 단위 */

	/* Interrupt를 허용할 때만 real_time_sleep()을 실행시킬 수 있음 */
	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick. Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
			 /* 하나의 timer tick을 기다렸다면 timer_sleep()을 통해 CPU를 다른 프로세스에게 양보함 */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		/* 하나의 tick 이내의 세밀한 시간을 다루기 위해서  */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));  /* 특정 시간(예: 밀리초) 동안 CPU를 점유하며 대기하기 위한 루프 수를 계산하여 busy_wait 함수에 전달하는 것입니다. */
	}
}
