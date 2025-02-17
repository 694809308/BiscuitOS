#include <linux/sched.h>
#include <linux/mm.h>

#include <asm/system.h>
#include <asm/io.h>

#include <linux/fdreg.h>

#define _S(nr)  (1 << ((nr)-1))
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

#define LATCH (1193180/HZ)

long user_stack[PAGE_SIZE >> 2];

struct {
	long *a;
	short b;
} stack_start = {
&user_stack[PAGE_SIZE >> 2], 0x10};

union task_union {
	struct task_struct task;
	char stak[PAGE_SIZE];
};

static union task_union init_task = { INIT_TASK, };

long volatile jiffies = 0;
long startup_time = 0;
struct task_struct *current = &(init_task.task);
struct task_struct *last_task_used_math = NULL;

struct task_struct *task[NR_TASKS] = { &(init_task.task), };

extern int timer_interrupt(void);
extern int system_call(void);

/*
 * schedule() is the scheduler function. This is GOOD CODE!
 * There probably won't be any reason to change this, as it should
 * work well in all circumstances (ie gives IO-bound processes good
 * response etc).
 * The one thing you might take a look at is the signal-handler
 * here.
 * NOTE! Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep.
 * The information in task[0] is never used.
 */
void schedule(void)
{
	int i, next, c;
	struct task_struct **p;

	/*
	 * Check alarm, wake up any interruptible tasks that have
	 * got a signal.
	 */
	for (p = &LAST_TASK; p > &FIRST_TASK; --p) {
		if (*p) {
			if ((*p)->alarm && (*p)->alarm < jiffies) {
				(*p)->signal |= (1 << (SIGALRM - 1));
				(*p)->alarm = 0;
			}
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			    (*p)->state == TASK_INTERRUPTIBLE)
				(*p)->state = TASK_RUNNING;
		}

		/* This is the schedule proper: */
		while (1) {
			c = -1;
			next = 0;
			i = NR_TASKS;
			p = &task[NR_TASKS];

			while (--i) {
				if (!*--p)
					continue;
				if ((*p)->state == TASK_RUNNING &&
				    (*p)->counter > c)
					c = (*p)->counter, next = i;
			}
			if (c)
				break;
			for (p = &LAST_TASK; p > &FIRST_TASK; --p)
				if (*p)
					(*p)->counter = ((*p)->counter >> 1) +
					    (*p)->priority;
		}
		switch_to(next);
	}
}

void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;

	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p;
	*p = current;
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();
	if (tmp)
		tmp->state = 0;
}

void wake_up(struct task_struct **p)
{
	if (p && *p) {
		(**p).state = 0;
		*p = NULL;
	}
}

void sched_init(void)
{
	int i;
	struct desc_struct *p;

	if (sizeof(struct sigaction) != 16)
		panic("Struct sigaction MUST be 16 bytes");
	set_tss_desc(gdt + FIRST_TSS_ENTRY, &(init_task.task.tss));
	set_ldt_desc(gdt + FIRST_LDT_ENTRY, &(init_task.task.ldt));
	p = gdt + 2 + FIRST_TSS_ENTRY;

	for (i = 1; i < NR_TASKS; i++) {
		task[i] = NULL;
		p->a = p->b = 0;
		p++;
		p->a = p->b = 0;
		p++;
	}
	/* Clear NT, so that we won't have trouble with that later on */
	__asm__("pushfl ; andl $0xffffbfff, (%esp) ; popfl");
	ltr(0);
	lldt(0);
	outb_p(0x36, 0x43);	/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff, 0x40);	/* LSB */
	outb(LATCH >> 8, 0x40);	/* MSB */
	set_intr_gate(0x20, &timer_interrupt);
	outb(inb_p(0x21) & ~0x01, 0x21);
	set_system_gate(0x80, &system_call);
}

#define TIME_REQUESTS 64

static struct timer_list {
	long jiffies;
	void (*fn) ();
	struct timer_list *next;
} timer_list[TIME_REQUESTS], *next_timer = NULL;

void add_timer(long jiffies, void (*fn) (void))
{
	struct timer_list *p;

	if (!fn)
		return;
	cli();
	if (jiffies <= 0)
		(fn) ();
	else {
		for (p = timer_list; p < timer_list + TIME_REQUESTS; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time request free");
		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;

		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;

	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p;
	*p = current;
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();
	if (tmp)
		tmp->state = 0;
}

/*
 * OK, here are some floppy things that should't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct *wait_motor[4] = { NULL, NULL, NULL, NULL };
static int mon_timer[4] = { 0, 0, 0, 0 };
static int moff_timer[4] = { 0, 0, 0, 0 };

unsigned char current_DOR = 0x0C;

int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr > 3)
		panic("floppy_on: nr>3");
	moff_timer[nr] = 10000;	/* 100s = very big :-) */
	cli();			/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR) {
		outb(mask, FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ / 2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr + wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr] = 3 * HZ;
}
