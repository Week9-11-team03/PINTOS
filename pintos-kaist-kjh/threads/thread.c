  #include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

static struct list sleep_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };


/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);
	list_init (&sleep_list);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

void thread_test_preemption(void) {
    if (!list_empty(&ready_list)) {
        struct thread *max_ready = list_entry(list_front(&ready_list), struct thread, elem);
        if (max_ready->priority > thread_current()->priority) {
            thread_yield();
        }
    }
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();
	

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock (t);

	// priority
	/*
	if (t->priority > thread_current()->priority) {
		thread_yield();
	}
	*/
	thread_test_preemption();
	
	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* 두 스레드의 우선순위를 비교하는 함수 (내림차순 정렬용) 
  
  @param a   : 비교할 첫 번째 list_elem (struct thread의 elem 멤버)
  @param b   : 비교할 두 번째 list_elem
  @param aux : 사용되지 않는 매개변수 (컴파일러 경고 방지용 UNUSED)
  @return    : a의 우선순위가 b보다 높으면 true, 그렇지 않으면 false
  
  ▶ list_insert_ordered()에서 우선순위 기반 정렬을 위해 사용됨
  ▶ ready_list, waiters 리스트 등 우선순위 정렬이 필요한 모든 곳에서 활용
  ▶ list_entry 매크로로 list_elem → struct thread 변환 후 priority 비교 */
bool thread_cmp_priority(const struct list_elem *a, 
                 const struct list_elem *b, 
                 void *aux UNUSED) {
    // list_elem으로부터 상위 구조체(struct thread) 포인터 추출
    const struct thread *t_a = list_entry(a, struct thread, elem);
    const struct thread *t_b = list_entry(b, struct thread, elem);
    
	if (t_a == NULL || t_b == NULL){
		return false;
	}

    // 높은 우선순위가 앞에 오도록 내림차순 정렬 (true: a가 b보다 우선)
    return t_a->priority > t_b->priority;
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	// list_push_back (&ready_list, &t->elem);		// FIFO 방식 
	list_insert_ordered(& ready_list, & t->elem, thread_cmp_priority, NULL);	// priority 방식
	t->status = THREAD_READY;
	intr_set_level (old_level);
}


// 두 스레드의 wake_up_tick 값을 비교해서
// a가 b보다 먼저 깨어나야 하면 true를 반환한다.
// → list_insert_ordered()에 넘겨줄 비교 함수로 사용됨
static bool compare_wakeup_tick(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
    // list_elem 포인터 a, b를 각각 thread 구조체로 캐스팅
    struct thread *t1 = list_entry(a, struct thread, elem);
    struct thread *t2 = list_entry(b, struct thread, elem);

    // 깨어나야 할 시각이 더 빠른 쪽이 앞으로 오게 정렬
    return t1->wakeup_tick < t2->wakeup_tick;
}

// 현재 실행 중인 스레드를 지정한 시간(wakeup_tick)까지 잠재운다.
// sleep_list에 wakeup_tick 기준으로 정렬하여 추가한다.
void thread_sleep(int64_t wakeup_tick) {
    struct thread *cur = thread_current();  // 지금 실행 중인 스레드를 가져온다

    // idle_thread(항상 돌아가는 기본 스레드)는 절대 잠들면 안 되므로 바로 반환
    if (cur == idle_thread)
        return;

    // sleep_list를 조작하는 동안 인터럽트를 잠깐 꺼서 예기치 않은 동시 접근을 막는다
    enum intr_level old_level = intr_disable();

    // 이 스레드가 언제 깨어나야 할지 기록해둔다
    cur->wakeup_tick = wakeup_tick;

    // sleep_list에 이 스레드를 wakeup_tick 기준으로 정렬해서 삽입한다
    // (가장 빨리 깨어날 스레드가 sleep_list의 맨 앞에 오게 됨)
    list_insert_ordered(&sleep_list, &cur->elem, compare_wakeup_tick, NULL);

    // 현재 스레드를 BLOCKED 상태로 바꾸고 CPU를 양보한다(실행 중단)
    thread_block();

    // sleep_list 조작이 끝났으니 인터럽트 상태를 원래대로 돌려놓는다
    intr_set_level(old_level);
}

// 현재 시각(current_tick)보다 먼저 깨어나야 할 스레드들을 깨운다.
// sleep_list는 wakeup_tick 기준으로 정렬되어 있다고 가정한다.
void thread_awake(int64_t current_tick) {
    // sleep_list의 맨 앞 요소부터 차례로 확인하기 위해 반복문 시작
    struct list_elem *e = list_begin(&sleep_list);

    // sleep_list의 끝에 도달할 때까지 반복
    while (e != list_end(&sleep_list)) {
        // 현재 리스트 요소(e)가 가리키는 스레드 구조체를 가져온다
        struct thread *t = list_entry(e, struct thread, elem);

        // 이 스레드가 깨어나야 할 시간이 되었는지 확인
        if (t->wakeup_tick <= current_tick) {
            // 깨어날 시간이 되었으므로, sleep_list에서 제거하고
            // list_remove는 다음 요소의 포인터를 반환한다
            e = list_remove(e);

            // 스레드를 READY 상태로 전환하여 실행 대기열에 넣는다
            thread_unblock(t);
        } else {
            // sleep_list가 wakeup_tick 오름차순으로 정렬되어 있으므로,
            // 아직 깨어날 시간이 안 된 스레드를 만나면 이후는 모두 더 늦은 시간임
            // 따라서 반복문을 종료한다
            break;
        }
    }
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) {
    // 현재 실행 중인 스레드 포인터 획득
    struct thread *curr = thread_current ();
    enum intr_level old_level;

    // 인터럽트 컨텍스트에서 호출 금지 검증
    // (인터럽트 핸들러 내부에서 yield 시도 방지)
    ASSERT (!intr_context ());

    // 인터럽트 비활성화 (동기화 보장)
    old_level = intr_disable ();

    // 현재 스레드가 idle 스레드가 아닌 경우에만 처리
    if (curr != idle_thread)
        // ready_list 끝에 현재 스레드 추가 (FIFO 방식)
        // list_push_back (&ready_list, &curr->elem);

		// priority의 맞게 현재 스레드 추가 (Priority 방식)
		list_insert_ordered(& ready_list, & curr->elem, thread_cmp_priority, NULL);	// priority 방식

    // 스케줄러 호출로 컨텍스트 스위칭 수행
    // THREAD_READY 상태로 전환되며 ready_list에서 다음 스레드 선택
    do_schedule (THREAD_READY);

    // 인터럽트 상태 복원
    intr_set_level (old_level);
}


/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority) {
    struct thread *cur = thread_current();
    
    // 원래 우선순위 저장
    cur->init_priority = new_priority;
    
    // 우선순위 기부 상황 확인 후 실제 우선순위 결정
    refresh_priority();
    
    // 선점 검사
    thread_test_preemption();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->wakeup_tick = 0;
	t->magic = THREAD_MAGIC;
	t->priority = priority;
	t->init_priority = priority;

	// 스레드 구조체의 hold_list를 초기화
	list_init(&t->hold_list);
	// 스레드 구조체의 wait on lock을 초기화
	t->wait_on_lock = NULL;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}

void donate_priority(void) {
    struct thread *cur_t = thread_current();           // 현재 실행 중인 스레드를 가져옴
    struct lock *cur_lock = cur_t->wait_on_lock;       // 현재 스레드가 기다리고 있는 락을 가져옴

    if (cur_lock == NULL) {                            // 기다리는 락이 없으면 기부할 필요가 없음
        return;
    }

    struct thread *holder = cur_lock->holder;          // 락을 소유하고 있는 스레드를 가져옴

    if (holder == NULL) {                              // 락의 소유자가 없으면 기부할 대상이 없음
        return;
    }

    while (cur_lock != NULL &&                         // 중첩 기부(nested donation) 처리를 위한 반복문
           cur_lock->holder != NULL &&                 // 락의 소유자가 있고
           cur_lock->holder->priority < cur_t->priority) { // 현재 스레드의 우선순위가 더 높을 때만 기부

        cur_lock->holder->priority = cur_t->priority;  // 락 소유자의 우선순위를 현재 스레드의 우선순위로 높임

        cur_t = cur_lock->holder;                      // 다음 단계 기부를 위해 현재 스레드를 락 소유자로 변경
        cur_lock = cur_t->wait_on_lock;                // 락 소유자가 기다리고 있는 다음 락으로 이동
    }
}

void refresh_priority(void) {
    struct thread *cur_t = thread_current();  // 현재 실행 중인 스레드를 가져옴
    int max_priority = cur_t->init_priority;  // 현재 스레드의 원래 우선순위(init_priority)로 시작

    // 현재 스레드가 보유한 모든 락(hold_list)을 순회
    struct list_elem *e;
    for (e = list_begin(&cur_t->hold_list); e != list_end(&cur_t->hold_list); e = list_next(e)) {
        // hold_list의 각 요소를 lock 구조체로 변환
        struct lock *l = list_entry(e, struct lock, elem);
        
        // 해당 락을 기다리는 스레드(waiters)가 있는지 확인
        if (!list_empty(&l->semaphore.waiters)) {
            // waiters 리스트는 우선순위로 정렬되어 있으므로, 맨 앞의 스레드가 가장 높은 우선순위를 가짐
            struct thread *t = list_entry(list_front(&l->semaphore.waiters), struct thread, elem);

            // 해당 스레드의 우선순위가 현재까지의 최대 우선순위보다 높으면 갱신
            if (t->priority > max_priority) {
                max_priority = t->priority;
            }
        }
    }

    // 계산된 최대 우선순위(원래 우선순위와 기부받은 우선순위 중 높은 값)를 현재 스레드에 적용
    cur_t->priority = max_priority;
}
