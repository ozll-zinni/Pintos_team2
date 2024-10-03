#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/filesys.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void check_address(void *addr);
void get_argument(void *rsp, int *arg, int count);
void halt(void);
void exit(int status);
bool create(const char *file, unsigned initial_size);
bool remove(const char *filename);
/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	uint64_t *sp = f->rsp;
	check_address((void *)sp);
	int syscall_number = *sp;

	printf ("system call!\n");
	switch(syscall_number){
		case SYS_HALT :	halt(); /* Halt the operating system. */
		case SYS_EXIT : exit(f->R.rdi); /* Terminate this process. */
		// case SYS_FORK : fork(); /* Clone current process. */
		// case SYS_EXEC : exec(); /* Switch current process. */
		// case SYS_WAIT : wait(); /* Wait for a child process to die. */
		case SYS_CREATE : {
			const char *filename = (const char *)f->R.rdi;
			unsigned initial_size = (unsigned)f->R.rsi;
			f->R.rax = create(filename, initial_size);
		}; /* Create a file. */
		case SYS_REMOVE : {
			char *filename = f->R.rdi;
			remove(filename);
		} /* Delete a file. */
		// case SYS_OPEN : open();  /* Open a file. */
		// case SYS_FILESIZE : filesize(); /* Obtain a file's size. */
		// case SYS_READ : read(); /* Read from a file. */
		// case SYS_WRITE : write();  /* Write to a file. */
		// case SYS_SEEK : seek(); /* Change position in a file. */
		// case SYS_TELL : tell(); /* Report current position in a file. */
		// case SYS_CLOSE : close(); /* Close a file. */
		// default : printf("이상한거 나옴");
	}
	thread_exit ();
}

void
check_address(void *addr){

	if(addr == NULL){
		exit(-1);
	}
	if(!is_user_vaddr(addr)){
		exit(-1);
	}
}

void 
halt(void){
	power_off();
}

void
exit(int status){
	struct thread *curr = thread_current();
	printf("%s:exit(%d)", curr->name, status);
	thread_exit();
}

bool
create(const char *filename, unsigned initial_size){
	return filesys_create(filename, initial_size);
}

bool
remove(const char *filename){
	return filesys_remove(filename);
}

