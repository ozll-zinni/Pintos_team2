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
#include "threads/synch.h"
#include "userprog/process.h"
#include "filesys/file.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);
void check_address(void *addr);
void get_argument(void *rsp, int *arg, int count);
void halt(void);
void exit(int status);
int exec(char *cmd_line);
int fork(const char *thread_name, struct intr_frame *f);
int wait(int pid);
bool create(const char *file, unsigned initial_size);
bool remove(const char *filename);
int open(const char *filename);
int read(int fd, void *buffer, unsigned size);
int filesize(int fd);
int write(int fd, const void *buffer, unsigned size);
void seek(int fd, unsigned position);
unsigned tell(int fd);
void close(int fd);
struct lock file_lock;
/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081			/* Segment selector msr */
#define MSR_LSTAR 0xc0000082		/* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void) {
    write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);
    write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

    lock_init(&file_lock);  // 파일 시스템 락 초기화
}


/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED)
{
    // 시스템 호출 번호는 rax 레지스터에 저장됨
    int syscall_number = f->R.rax;
    printf("System call number: %d\n", syscall_number);

    switch (syscall_number)
    {
    case SYS_HALT:
        halt(); /* Halt the operating system. */
        break;
    case SYS_EXIT:
        exit(f->R.rdi); /* Terminate this process. */
        break;
    case SYS_FORK:
        f->R.rax = fork(f->R.rdi, f);
        break;
    case SYS_EXEC:
        f->R.rax = exec(f->R.rdi);
        break;
    case SYS_WAIT:
        f->R.rax = wait(f->R.rdi);
        break;
    case SYS_CREATE:
    {
        const char *filename = (const char *)f->R.rdi;
        check_address(filename); // 파일 이름 유효성 검사
        unsigned initial_size = (unsigned)f->R.rsi;
        f->R.rax = create(filename, initial_size);
        break;
    }
    case SYS_REMOVE:
    {
        const char *filename = (const char *)f->R.rdi;
        f->R.rax = remove(filename);
        break;
    }
    case SYS_OPEN:
        f->R.rax = open(f->R.rdi);
        break;
    case SYS_FILESIZE:
        f->R.rax = filesize(f->R.rdi);
        break;
    case SYS_READ:
        f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
        break;
    case SYS_WRITE:
        f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
        break;
    case SYS_SEEK:
        seek(f->R.rdi, f->R.rsi);
        break;
    case SYS_TELL:
        f->R.rax = tell(f->R.rdi);
        break;
    case SYS_CLOSE:
        close(f->R.rdi);
        break;
    default:
    {
        printf("Invalid system call number. \n");
        exit(-1);
    }
    }
}


void check_address(void *addr)
{

	if (addr == NULL || !is_user_vaddr(addr))
	{
		printf("Invalid address: %p\n", addr);
		exit(-1);
	}
}

void halt(void)
{
	printf("Halt called, shutting down...\n");
	power_off();
}

void exit(int status)
{
	struct thread *curr = thread_current();
	curr->exit_status = status;
	printf("%s: exit%d\n", curr->name, status);
	thread_exit(); // 정상적으로 종료되었으면 0
}

int exec(char *cmd_line)
{
	// cmd_line이 유효한 사용자 주소인지 확인 -> 잘못된 주소인 경우 종료/예외 발생
	check_address(cmd_line);

	// process.c 파일의 process_create_initd 함수와 유사하다.
	// 단, 스레드를 새로 생성하는 건 fork에서 수행하므로
	// exec는 이미 존재하는 프로세스의 컨텍스트를 교체하는 작업을 하므로
	// 현재 프로세스의 주소 공간을 교체하여 새로운 프로그램을 실행
	// 이 함수에서는 새 스레드를 생성하지 않고 process_exec을 호출한다.

	// process_exec 함수 안에서 filename을 변경해야 하므로
	// 커널 메모리 공간에 cmd_line의 복사본을 만든다.
	// (현재는 const char* 형식이기 때문에 수정할 수 없다.)
	char *cmd_line_copy;
	cmd_line_copy = palloc_get_page(0);
	if (cmd_line_copy == NULL)
		exit(-1);							  // 메모리 할당 실패 시 status -1로 종료한다.
	strlcpy(cmd_line_copy, cmd_line, PGSIZE); // cmd_line을 복사한다.

	// 스레드의 이름을 변경하지 않고 바로 실행한다.
	if (process_exec(cmd_line_copy) == -1)
		exit(-1); // 실패 시 status -1로 종료한다.
	return 0;
}

bool create(const char *filename, unsigned initial_size)
{
	return filesys_create(filename, initial_size);
}

bool remove(const char *filename)
{
	return filesys_remove(filename);
}

int open(const char *filename)
{

	/* 파일을 open */
	/* 해당 파일 객체에 파일 디스크립터 부여 */
	/* 파일 디스크립터 리턴 */
	/* 해당 파일이 존재하지 않으면-1 리턴 */

	// 사용자로부터 전달된 filename 포인터가 유효한지 검증
	if (filename == NULL || !is_user_vaddr(filename))
	{
		return -1; // 유효하지 않은 파일 이름일 경우
	}

	// 파일 열기 시도
	struct file *file = filesys_open(filename);
	if (file == NULL)
	{
		return -1; // 파일을 열지 못했을 경우
	}

	// 현재 스레드의 파일 디스크립터 테이블에 파일 추가
	struct thread *cur = thread_current();
	int fd = process_add_file(file);
	if (fd == -1)
	{
		file_close(file); // 파일 디스크립터 할당에 실패하면 파일을 닫음
	}
	return fd; // 성공적으로 파일을 열었으면 fd 반환
}

// int exec(char *cmd_line){
//     // cmd_line이 유효한 사용자 주소인지 확인 -> 잘못된 주소인 경우 종료/예외 발생
//     check_address(cmd_line);
//     // process.c 파일의 process_create_initd 함수와 유사하다.
//     // 단, 스레드를 새로 생성하는 건 fork에서 수행하므로
//     // exec는 이미 존재하는 프로세스의 컨텍스트를 교체하는 작업을 하므로
//     // 현재 프로세스의 주소 공간을 교체하여 새로운 프로그램을 실행
//     // 이 함수에서는 새 스레드를 생성하지 않고 process_exec을 호출한다.
//     // process_exec 함수 안에서 filename을 변경해야 하므로
//     // 커널 메모리 공간에 cmd_line의 복사본을 만든다.
//     // (현재는 const char* 형식이기 때문에 수정할 수 없다.)
//     char *cmd_line_copy;
//     cmd_line_copy = palloc_get_page(0);
//     if (cmd_line_copy == NULL)
//         exit(-1);                             // 메모리 할당 실패 시 status -1로 종료한다.
//     strlcpy(cmd_line_copy, cmd_line, PGSIZE); // cmd_line을 복사한다.
//     // 스레드의 이름을 변경하지 않고 바로 실행한다.
//     if (process_exec(cmd_line_copy) == -1)
//         exit(-1); // 실패 시 status -1로 종료한다.
// }

int read(int fd, void *buffer, unsigned size) {
    struct thread *curr = thread_current();
    struct file *file = curr->fd_table[fd];
    int file_bytes = 0;  // file_bytes 초기화
    if(fd < 0 || fd >= MAX_FD) {
        return -1;
    }
    
    if (fd == 0) {
        for(unsigned i = 0; i < size; i++) {
            ((uint8_t *)buffer)[i] = input_getc();
        }
        file_bytes = size;
    } else if(fd >= 2) {
        lock_acquire(&file_lock);
        file_bytes = (int)file_read(file, buffer, size);
        lock_release(&file_lock);
    } else if (fd == 1) {
        return -1;
    }
    
    return file_bytes;
}


int filesize(int fd) {
    struct file *file = process_get_file(fd);
    if (file == NULL) {
        return -1;
    }
    return file_length(file);
}


// buffer로부터 사이즈 쓰기
int write(int fd, const void *buffer, unsigned size) {
	check_address(buffer);

	int write_result;
	lock_acquire(&file_lock);
	if (fd == 1) {
		putbuf(buffer, size);		// 문자열을 화면에 출력하는 함수
		write_result = size;
	}
	else {
		if (process_get_file(fd) != NULL) {
			write_result = file_write(process_get_file(fd), buffer, size);
		}
		else {
			write_result = -1;
		}
	}
	lock_release(&file_lock);
	return write_result;
}

void seek(int fd, unsigned position)
{
	struct file *file = process_get_file(fd);
	file_seek(file, position);
}

unsigned
tell(int fd)
{
	struct file *file = process_get_file(fd);
	file_tell(file);
}

void close(int fd)
{
	struct file *file = process_get_file(fd);
	file_close(&file);
}

int fork(const char *thread_name, struct intr_frame *f)
{
	return process_fork(thread_name, f);
}

int wait(int pid)
{
	return process_wait(pid);
}
