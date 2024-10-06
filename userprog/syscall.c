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
#include "userprog/process.h"
#include "filesys/file.h"
#include "threads/palloc.h"

struct lock filesys_lock;
void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void check_address(void *addr);

void get_argument(void *rsp, int *arg, int count);
void halt(void);
void exit(int status);
tid_t exec(char *cmd_line);
int fork(const char * thread_name, struct intr_frame *f);
int wait(int pid);
bool create(const char *file, unsigned initial_size);
bool remove(const char *filename);
int open(const char *filename);
int read (int fd, void *buffer, unsigned size);
int filesize(int fd);
int write (int fd, void *buffer, unsigned size);
void seek(int fd, unsigned position);
unsigned tell(int fd);
void close(int fd);

void process_close_file(int fd);
struct file *process_get_file(int fd);
int process_add_file(struct file *f);
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

const int STDIN = 1;
const int STDOUT = 2;

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
    lock_init(&filesys_lock);
}

void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	
	int sys_number = f->R.rax;
	switch (sys_number){

		case SYS_HALT:			/* Halt the operating system. */
			halt();
			break;
		
		case SYS_EXIT:			/* Terminate this process. */
			exit(f->R.rdi);
			break;

		case SYS_FORK:			/* Clone current process. */
			f->R.rax = fork(f->R.rdi, f);
			break;
			                    
		case SYS_EXEC:			/* Switch current process. */
			 if(exec(f->R.rdi) == -1)
			 	exit(-1);
           	 break;
	
		case SYS_WAIT:			/* Wait for a child process to die. */
			 f->R.rax = wait(f->R.rdi);
			 break; 

	    case SYS_CREATE:		/* Create a file. */
			 f->R.rax = create(f->R.rdi, f->R.rsi);
			 break;

		case SYS_REMOVE:		/* Delete a file. */
			 f->R.rax = remove(f->R.rdi);
			 break;

		case SYS_OPEN:			/* Open a file. */
			 f->R.rax = open(f->R.rdi);
			 break;	              
	               
        case SYS_FILESIZE: 		/* Obtain a file's size. */
			 f->R.rax = filesize(f->R.rdi);
			 break;

	    case SYS_READ:			/* Read from a file. */
			 f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
			 break;
	
		case SYS_WRITE:			/* Write to a file. */
			 f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
			 break;
	                   
		case SYS_SEEK:			/* Change position in a file. */
			 seek(f->R.rdi, f->R.rsi);
			 break;
	                  
        case SYS_TELL:			/* Report current position in a file. */
			 f->R.rax = tell(f->R.rdi);
			 break;
	                   
		case SYS_CLOSE:			/* Close a file. */
			 close(f->R.rdi);
			 break;

		default:
			// printf ("system call!\n");
			// thread_exit ();
			exit(-1);
			break;
		            
	}
	
}


void
check_address(void *addr){

if (addr == NULL || !is_user_vaddr(addr)) {
	//  printf("Invalid address: %p\n", addr);
    exit(-1);
}
}

void 
halt(void){
	// printf("Halt called, shutting down...\n");
	power_off();
}

void
exit(int status){
	struct thread *curr = thread_current();
	curr->exit_status = status;
	printf("%s: exit(%d)\n", curr->name, status);
	thread_exit(); // 정상적으로 종료되었으면 0
}

bool
create(const char *filename, unsigned initial_size){
    check_address(filename);
	if(filesys_create(filename, initial_size)){
        return true;
    } else{
        return false;
    }

}

bool
remove(const char *filename){
    check_address(filename);
	return filesys_remove(filename);
}

int open(const char *filename)
{

/* 파일을 open */
/* 해당 파일 객체에 파일 디스크립터 부여 */
/* 파일 디스크립터 리턴 */
/* 해당 파일이 존재하지 않으면-1 리턴 */
    check_address(filename);
    lock_acquire(&filesys_lock);
    
    struct file *file = filesys_open(filename);

    if(file == NULL){
        return -1;
    }

    int fd = process_add_file(file);

    if(fd==-1){
        file_close(file);
    }
    lock_release(&filesys_lock);
    return fd;

}

tid_t
exec(char *cmd_line){
    check_address(cmd_line);
	// process.c의 process_created_initd와 유사함
	// 스레드를 생성하는건 fork에서 하므로, 이 함수에서는 새 스레드를 생성하지 않고 process_exec을 호출한다

	// process_exec에서 filename을 변경해야 하므로
	// 커널 메모리 공간에 cmd_line의 복사본을 만든다
	int size = strlen(cmd_line) +1;
	char *cmd_line_copy = palloc_get_page(PAL_ZERO);
	if (cmd_line_copy == NULL){
		exit(-1);
	}
	// 메모리 할당 실패 시 status -1로 종료한다
	strlcpy(cmd_line_copy, cmd_line, size);

	if(process_exec(cmd_line_copy) == -1){
		return -1;
	}
	NOT_REACHED();
	return 0;
}

int read (int fd, void *buffer, unsigned size)
 {
	check_address(buffer);
	check_address(buffer+size-1);
	struct thread *curr = thread_current();
	struct file *file = process_get_file(fd);
	unsigned char *buf = buffer;
	int file_bytes;

	if(file == STDIN)
	{
		char key;
		for(int file_bytes = 0; file_bytes < size; file_bytes++){
			key = input_getc();
			*buf++ = key;
			if(key == '\0'){
				break;
			}
		}
	} else if(file == STDOUT){
		return -1;
	} else{
		lock_acquire(&filesys_lock);
		file_bytes = file_read(file,buffer,size);
		lock_release(&filesys_lock);
	}
	return file_bytes;

 /* 파일에 동시 접근이 일어날 수 있으므로 Lock 사용 */
 /* 파일 디스크립터를 이용하여 파일 객체 검색 */
 /* 파일 디스크립터가 0일 경우 키보드에 입력을 버퍼에 저장 후
버퍼의 저장한 크기를 리턴 (input_getc() 이용) */
 /* 파일 디스크립터가 0이 아닐 경우 파일의 데이터를 크기만큼 저
장 후 읽은 바이트 수를 리턴 */
 }

 int 
 filesize(int fd){
	struct file *curr = process_get_file(fd);
	if(curr == NULL){
		return -1;
	}
	return file_length(curr);
 }

int write(int fd, void *buffer, unsigned size)
{
	check_address(buffer);
	int read_count; // 글자수 카운트 용(for문 사용하기 위해)
	struct file *file_obj = process_get_file(fd);
	unsigned char *buf = buffer;

	if (file_obj == NULL)
		return -1;
	
	/* STDOUT일 때 */
	if(file_obj == STDOUT)
	{
		putbuf(buffer, size); // fd값이 1일 때, 버퍼에 저장된 데이터를 화면에 출력(putbuf()이용)
		read_count = size;
		
	}
	/* STDIN일 때 : -1 반환 */
	else if (file_obj == STDIN)
	{
		
		return -1;
	}
	
	else {
			lock_acquire(&filesys_lock);
			read_count = file_write(file_obj,buffer, size);
			lock_release(&filesys_lock);
	}
	return read_count;

}


void 
seek(int fd, unsigned position){
	struct file *file = process_get_file(fd);
	// if(fd < 2){
	// 	return;
	// }
	// file_seek(&file, position);

	if(file != NULL){
		file_seek(file, position);
	}
}

unsigned 
tell (int fd){
	struct file *file = process_get_file(fd);
	if(fd<2){
		return;
	}
	return file_tell(&file);
}

void
close(int fd){
	if(fd <= 1) return;
	struct file *file = process_get_file(fd);
	if(file == NULL)
	{
		return;
	}
	process_close_file(fd);
}

int fork(const char * thread_name, struct intr_frame *f)
{
	return process_fork(thread_name, f);
}

int wait(int pid)
{
	return process_wait(pid);
}

int 
process_add_file(struct file *f){
    struct thread *curr = thread_current();
    struct f **fdt = curr->fd_table;
    // fd_table에서 빈 자리를 찾아 파일 포인터 저장
    while(curr->fd < MAX_FD && fdt[curr->fd])
    {
        curr->fd++;
    }

    if(curr->fd >= MAX_FD)
    {
        return -1;
    }
    fdt[curr->fd] = f;
    return curr->fd;
}

struct 
file *process_get_file(int fd){
	struct thread *curr = thread_current(); // 현재 스레드(프로세스) 가져오기

    // fd가 유효한지 확인
    if (fd < 0 || fd >= MAX_FD) { // FD_COUNT는 파일 디스크립터의 최대 개수
        return NULL; // 유효하지 않은 fd일 경우 NULL 반환
    }

    return curr->fd_table[fd]; // 해당 fd의 파일 객체 반환
}

void
process_close_file(int fd){
	struct thread *curr = thread_current();
	if(fd<0 || fd>=MAX_FD){
		return;
	}
	curr->fd_table[fd] = NULL;
}
