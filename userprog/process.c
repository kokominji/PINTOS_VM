#include "userprog/process.h"

#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "include/lib/user/syscall.h"
#include "intrinsic.h"
#include "lib/user/syscall.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/check_perm.h"
#include "userprog/gdt.h"
#include "userprog/tss.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup(void);
static bool load(const char *file_name, char *args, struct intr_frame *if_);
static void initd(void *f_name);
static void __do_fork(void *);
static uint64_t *push_stack(char *arg, size_t size, struct intr_frame *if_);
static uint64_t *pop_stack(size_t size, struct intr_frame *if_);

/**
 * @brief fork 작업에 필요한 데이터를 전달하기 위한 구조체
 * @branch feat/fork_handler
 * @details 부모 프로세스의 정보와 인터럽트 프레임을 자식 프로세스 생성 함수에 전달하기 위해
 * 사용됩니다.
 */
struct fork_data {
    struct thread *parent;        /**< 부모 스레드 포인터 */
    struct intr_frame *parent_if; /**< 부모의 인터럽트 프레임 포인터 */
};

struct init_data {
    struct thread *parent;
    const char *file_name;
};

/* General process initializer for initd and other process. */
static void process_init(void) {
    struct thread *current = thread_current();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t process_create_initd(const char *file_name) {
    char *fn_copy;
    tid_t tid;
    struct init_data init_data;
    struct thread *t = thread_current();

    /* Make a copy of FILE_NAME.
     * Otherwise there's a race between the caller and load(). */
    fn_copy = palloc_get_page(0);
    if (fn_copy == NULL)
        return TID_ERROR;
    strlcpy(fn_copy, file_name, PGSIZE);
    init_data.file_name = fn_copy;
    init_data.parent = t;

    char *saveptr = NULL;
    char *file_cut = strtok_r(file_name, " ", &saveptr);

    /* Create a new thread to execute FILE_NAME. */
    tid = thread_create(file_cut, PRI_DEFAULT, initd, &init_data);
    sema_down(&t->wait_sema);
    if (tid == TID_ERROR)
        palloc_free_page(fn_copy);
    return tid;
}

/* A thread function that launches first user process. */
static void initd(void *init_data_) {
#ifdef VM
    supplemental_page_table_init(&thread_current()->spt);
#endif
    process_init();
    struct init_data *init_data = (struct init_data *)init_data_;
    struct thread *t = thread_current();
    t->parent = init_data->parent;
    list_push_back(&t->parent->childs, &t->sibling_elem);
    sema_up(&t->parent->wait_sema);
    if (process_exec(init_data->file_name) < 0)
        PANIC("Fail to launch initd\n");
    NOT_REACHED();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
/**
 * @brief 현재 프로세스를 복제하여 새로운 프로세스를 생성합니다.
 * @branch feat/fork_handler
 * @param name 새 프로세스의 이름
 * @param if_ 부모 프로세스의 인터럽트 프레임
 * @return 성공 시 자식 프로세스의 TID, 실패 시 TID_ERROR
 * 이 함수는 현재 실행 중인 프로세스의 메모리와 상태를 복사하여
 * 새로운 자식 프로세스를 생성합니다. 부모 프로세스는 자식 프로세스의
 * 생성이 완료될 때까지 세마포어를 통해 대기
 * @details
 * 1. fork_data 구조체를 할당하여 부모 정보와 인터럽트 프레임을 저장
 * 2. thread_create를 통해 __do_fork 함수를 실행할 새 스레드 생성
 * 3. 세마포어를 통해 자식 프로세스 생성 완료 대기
 * 4. 자식 프로세스의 TID 반환
 * @note 메모리 할당 실패 시 TID_ERROR를 반환합니다.
 * @see __do_fork()
 */
tid_t process_fork(const char *name, struct intr_frame *if_) {
    /* Clone current thread to new thread.*/
    struct thread *curr = thread_current();

    struct fork_data *fork_data = malloc(sizeof(struct fork_data));
    fork_data->parent = curr;
    fork_data->parent_if = if_;

    tid_t tid = thread_create(name, PRI_DEFAULT, __do_fork, fork_data);
    if (tid == TID_ERROR) {
        free(fork_data);
        return TID_ERROR;
    }

    sema_down(&curr->fork_sema);
    if (list_empty(&curr->childs)) {
        return -1;
    }
    struct thread *t = list_entry(list_back(&curr->childs), struct thread, sibling_elem);
    if (t->exit_status == -1) {
        return -1;
    }
    return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool duplicate_pte(uint64_t *pte, void *va, void *aux) {
    struct thread *current = thread_current();
    struct thread *parent = (struct thread *)aux;
    void *parent_page;
    void *newpage;
    bool writable;

    /* 1. If the parent_page is kernel page, then return immediately. */
    if (!is_user_vaddr(va)) {
        return true;
    }

    /* 2. Resolve VA from the parent's page map level 4. */
    parent_page = pml4_get_page(parent->pml4, va);

    /* 3. Allocate new PAL_USER page for the child and set result to
     *    NEWPAGE. */
    if (newpage = pml4_get_page(current->pml4, va) == NULL) {
        newpage = palloc_get_page(PAL_USER | PAL_ZERO);
    }
    if (newpage == NULL) {
        return false;
    }

    /* 4. Duplicate parent's page to the new page and
     *    check whether parent's page is writable or not (set WRITABLE
     *    according to the result). */
    writable = is_writable(pte);
    pml4_set_page(current->pml4, (uint8_t *)pg_round_down(va), newpage, writable);
    memcpy(newpage, parent_page, PGSIZE);

    /* 5. Add new page to child's page table at address VA with WRITABLE
     *    permission. */
    if (!pml4_set_page(current->pml4, va, newpage, writable)) {
        /* 6. if fail to insert page, do error handling. */
        palloc_free_page(newpage);
        return false;
    }
    return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
/**
 * @brief 부모 프로세스의 실행 컨텍스트를 복사하여 자식 프로세스를 생성합니다.
 *
 * @branch feat/fork_handler
 * @param aux fork_data 구조체 포인터 (부모 정보와 인터럽트 프레임 포함)
 *
 * 이 함수는 새로 생성된 스레드에서 실행되며, 부모 프로세스의 메모리와
 * 상태를 복사하여 자식 프로세스를 완전히 생성합니다.
 *
 * @details
 * 1. 부모-자식 관계 설정 (parent, childs 리스트)
 * 2. 부모의 인터럽트 프레임을 자식에게 복사
 * 3. 페이지 테이블 생성 및 메모리 복사
 * 4. 파일 디스크립터 복사 (TODO)
 * 5. 프로세스 초기화
 * 6. 부모에게 완료 신호 전송
 * 7. 자식 프로세스 실행 시작
 *
 * @note 부모 프로세스는 이 함수가 완료될 때까지 대기합니다.
 * @see process_fork()
 */
static void __do_fork(void *aux) {
    struct intr_frame if_;
    struct fork_data *fork_data = (struct fork_data *)aux;
    struct thread *parent = fork_data->parent;
    struct thread *current = thread_current();
    /* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
    struct intr_frame *parent_if = fork_data->parent_if;
    bool succ = true;

    /* 부모-자식 관계 설정 */
    current->parent = parent;
    list_push_back(&parent->childs, &current->sibling_elem);

    /* 1. Read the cpu context to local stack. */
    memcpy(&if_, parent_if, sizeof(struct intr_frame));

    /* 2. Duplicate PT */
    current->pml4 = pml4_create();
    if (current->pml4 == NULL)
        goto error;

    process_activate(current);

#ifdef VM
    supplemental_page_table_init(&current->spt);
    if (!supplemental_page_table_copy(&current->spt, &parent->spt))
        goto error;
#else
    if (parent->pml4 && !pml4_for_each(parent->pml4, duplicate_pte, parent))
        goto error;
#endif

    /* TODO: Your code goes here.
     * TODO: Hint) To duplicate the file object, use `file_duplicate`
     * TODO:       in include/filesys/file.h. Note that parent should not return
     * TODO:       from the fork() until this function successfully duplicates
     * TODO:       the resources of parent.*/
    // 부모의 파일 디스크립터 테이블 복사
    current->fd_pg_cnt = parent->fd_pg_cnt;
    current->open_file_cnt = 0;

    ASSERT(current->fdt != NULL);
    if (current->fd_pg_cnt != 0) {
        palloc_free_page(current->fdt);
        current->fdt = palloc_get_multiple(PAL_ZERO, current->fd_pg_cnt);
        if (current->fdt == NULL) {
            goto error;
        }

        int i = 0;
        for (; current->open_file_cnt < parent->open_file_cnt; i++) {
            if (parent->fdt[i] != NULL) {
                current->fdt[i] = duplicate_file(parent->fdt[i]);
                if (current->fdt[i] == NULL) {
                    goto error;
                }
                current->open_file_cnt++;
            }
        }
    }

    process_init();

    free(fork_data);
    if_.R.rax = 0;  // 자식 rax 초기화

    /* Finally, switch to the newly created process. */
    if (succ) {
        sema_up(&(current->parent->fork_sema));
        do_iret(&if_);
    }
error:
    free(fork_data);
    current->exit_status = -1;
    thread_exit();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int process_exec(void *f_name) {
    //  $feat/arg-parse
    char *args;
    char *file_name = strtok_r(f_name, " ", &args);
    //  feat/arg-parse
    bool success;

    /* We cannot use the intr_frame in t3e thread structure.
     * This is because when current thread rescheduled,
     * it stores the execution information to the member. */
    struct intr_frame _if;
    _if.ds = _if.es = _if.ss = SEL_UDSEG;
    _if.cs = SEL_UCSEG;
    _if.eflags = FLAG_IF | FLAG_MBS;

    /* We first kill the current context */
    process_cleanup();

    /* And then load the binary */
    success = load(file_name, args, &_if);
    /* If load failed, quit. */
    palloc_free_page(f_name);
    if (!success) {
        thread_current()->exit_status = -1;
        thread_exit();
    }

    /* Start switched process. */
    do_iret(&_if);
    NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int process_wait(tid_t child_tid) {
    int child_exit_status = -1;
    struct thread *curr = thread_current();
    for (struct list_elem *e = list_begin(&curr->childs); e != list_end(&curr->childs);
         e = list_next(e)) {
        struct thread *t = list_entry(e, struct thread, sibling_elem);
        if (t && t->tid == child_tid) {
            // sema_down(&t->exit_sema);
            sema_down(&t->wait_sema);
            barrier();
            child_exit_status = t->exit_status;
            list_remove(&t->sibling_elem);
            sema_up(&t->exit_sema);
            break;
        }
    }
    return child_exit_status;
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void) {
    struct thread *cur = thread_current();
    if (cur->fd_pg_cnt != 0) {
        int i = 0;
        for (; cur->open_file_cnt > 0; i++) {
            barrier();
            remove_fd(i);
        }
        palloc_free_multiple(cur->fdt, cur->fd_pg_cnt);
    }
    bool is_user = is_user_thread();
    process_cleanup();
    if (cur->parent != NULL) {
        if (is_user) {
            printf("%s: exit(%d)\n", cur->name, cur->exit_status);
        }
        if (list_back(&cur->parent->childs) == &(cur->sibling_elem) &&
            !list_empty(&cur->parent->fork_sema.waiters)) {
            sema_up(&cur->parent->fork_sema);
        }
        sema_up(&cur->wait_sema);
        sema_down(&cur->exit_sema);
    }
}

/* Free the current process's resources. */
static void process_cleanup(void) {
    struct thread *curr = thread_current();

#ifdef VM
    supplemental_page_table_kill(&curr->spt);
#endif

    uint64_t *pml4;
    /* Destroy the current process's page directory and switch back
     * to the kernel-only page directory. */
    pml4 = curr->pml4;
    if (pml4 != NULL) {
        /* Correct ordering here is crucial.  We must set
         * cur->pagedir to NULL before switching page directories,
         * so that a timer interrupt can't switch back to the
         * process page directory.  We must activate the base page
         * directory before destroying the process's page
         * directory, or our active page directory will be one
         * that's been freed (and cleared). */

        curr->pml4 = NULL;
        pml4_activate(NULL);
        pml4_destroy(pml4);
    }
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void process_activate(struct thread *next) {
    /* Activate thread's page tables. */
    pml4_activate(next->pml4);

    /* Set thread's kernel stack for use in processing interrupts. */
    tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct ELF64_PHDR {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack(struct intr_frame *if_);
static bool validate_segment(const struct Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool load(const char *file_name, char *args, struct intr_frame *if_) {
    struct thread *t = thread_current();
    struct ELF ehdr;
    struct File *file_a = NULL;
    struct file *file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;

    /* Allocate and activate page directory. */
    t->pml4 = pml4_create();
    if (t->pml4 == NULL)
        goto done;
    process_activate(thread_current());

    /* Open executable file. */
    file_a = open_file(file_name);
    if (file_a == NULL) {
        printf("load: %s: open failed\n", file_name);
        goto done;
    }
    file = file_a->file_ptr;

    /* Read and verify executable header. */
    if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
        memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 ||
        ehdr.e_machine != 0x3E  // amd64
        || ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) || ehdr.e_phnum > 1024) {
        printf("load: %s: error loading executable\n", file_name);
        goto done;
    }

    /* Read program headers. */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++) {
        struct Phdr phdr;

        if (file_ofs < 0 || file_ofs > file_length(file))
            goto done;
        file_seek(file, file_ofs);

        if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
            goto done;
        file_ofs += sizeof phdr;
        switch (phdr.p_type) {
            case PT_NULL:
            case PT_NOTE:
            case PT_PHDR:
            case PT_STACK:
            default:
                /* Ignore this segment. */
                break;
            case PT_DYNAMIC:
            case PT_INTERP:
            case PT_SHLIB:
                goto done;
            case PT_LOAD:
                if (validate_segment(&phdr, file)) {
                    bool writable = (phdr.p_flags & PF_W) != 0;
                    uint64_t file_page = phdr.p_offset & ~PGMASK;
                    uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
                    uint64_t page_offset = phdr.p_vaddr & PGMASK;
                    uint32_t read_bytes, zero_bytes;
                    if (phdr.p_filesz > 0) {
                        /* Normal segment.
                         * Read initial part from disk and zero the rest. */
                        read_bytes = page_offset + phdr.p_filesz;
                        zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
                    } else {
                        /* Entirely zero.
                         * Don't read anything from disk. */
                        read_bytes = 0;
                        zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
                    }
                    if (!load_segment(file, file_page, (void *)mem_page, read_bytes, zero_bytes,
                                      writable))
                        goto done;
                } else
                    goto done;
                break;
        }
    }

    /* Set up stack. */
    if (!setup_stack(if_))
        goto done;

    /* Start address. */
    if_->rip = ehdr.e_entry;

    //  $feat/arg-parse
    char *argv[LOADER_ARGS_LEN / 2];
    uintptr_t stack_ptr[LOADER_ARGS_LEN / 2];
    argv[0] = file_name;
    uint8_t argc = 1;
    char *save_ptr = NULL;
    argv[argc] = strtok_r(args, " ", &save_ptr);
    while (argv[argc] != NULL) {
        argv[++argc] = strtok_r(NULL, " ", &save_ptr);
    }

    size_t total_mod8 = 0;
    size_t arg_len;
    for (int i = argc - 1; i >= 0; i--) {
        arg_len = strlen(argv[i]) + 1;
        stack_ptr[i] = push_stack(argv[i], arg_len, if_);
        total_mod8 = (total_mod8 + arg_len) % 8;
    }

    push_stack(NULL, (8 - total_mod8) % 8, if_);

    push_stack(NULL, sizeof(uintptr_t), if_);
    for (int i = argc - 1; i >= 0; i--) {
        push_stack((char *)(&stack_ptr[i]), sizeof(uintptr_t), if_);
    }
    push_stack(NULL, sizeof(uintptr_t), if_);
    if_->R.rsi = sizeof(uintptr_t) + if_->rsp;
    if_->R.rdi = argc;
    //  feat/arg-parse

    file_deny_write(file_a->file_ptr);
    int fd = set_fd(file_a);
    if (fd == -1) {
        goto done;
    }
    // remove_if_duplicated(fd);
    success = true;
done:
    return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Phdr *phdr, struct file *file) {
    /* p_offset and p_vaddr must have the same page offset. */
    if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
        return false;

    /* p_offset must point within FILE. */
    if (phdr->p_offset > (uint64_t)file_length(file))
        return false;

    /* p_memsz must be at least as big as p_filesz. */
    if (phdr->p_memsz < phdr->p_filesz)
        return false;

    /* The segment must not be empty. */
    if (phdr->p_memsz == 0)
        return false;

    /* The virtual memory region must both start and end within the
       user address space range. */
    if (!is_user_vaddr((void *)phdr->p_vaddr))
        return false;
    if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz)))
        return false;

    /* The region cannot "wrap around" across the kernel virtual
       address space. */
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
        return false;

    /* Disallow mapping page 0.
       Not only is it a bad idea to map page 0, but if we allowed
       it then user code that passed a null pointer to system calls
       could quite likely panic the kernel by way of null pointer
       assertions in memcpy(), etc. */
    if (phdr->p_vaddr < PGSIZE)
        return false;

    /* It's okay. */
    return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page(void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    file_seek(file, ofs);
    while (read_bytes > 0 || zero_bytes > 0) {
        /* Do calculate how to fill this page.
         * We will read PAGE_READ_BYTES bytes from FILE
         * and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* Get a page of memory. */
        uint8_t *kpage = palloc_get_page(PAL_USER);
        if (kpage == NULL)
            return false;

        /* Load this page. */
        if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
            palloc_free_page(kpage);
            return false;
        }
        memset(kpage + page_read_bytes, 0, page_zero_bytes);

        /* Add the page to the process's address space. */
        if (!install_page(upage, kpage, writable)) {
            printf("fail\n");
            palloc_free_page(kpage);
            return false;
        }

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool setup_stack(struct intr_frame *if_) {
    uint8_t *kpage;
    bool success = false;

    kpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (kpage != NULL) {
        success = install_page(((uint8_t *)USER_STACK) - PGSIZE, kpage, true);
        if (success)
            if_->rsp = USER_STACK;
        else
            palloc_free_page(kpage);
    }
    return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool install_page(void *upage, void *kpage, bool writable) {
    struct thread *t = thread_current();

    /* Verify that there's not already a page at that virtual
     * address, then map our page there. */
    return (pml4_get_page(t->pml4, upage) == NULL &&
            pml4_set_page(t->pml4, upage, kpage, writable));
}

/**
 * @brief 사용자 스택에 데이터를 푸시(push)합니다.
 *
 * @branch feat/arg-parse
 * @see https://www.notion.so/jactio/arg-parsing-235c9595474e8034af80c8ca37af7dd2?source=copy_link
 * @param arg 푸시할 데이터가 저장된 버퍼의 포인터입니다. NULL이면 0 값으로 채웁니다.
 * @param size 푸시할 바이트 수이며, 0보다 커야 합니다.
 * @param if_ 스택 포인터(rsp)를 조정할 intr_frame 구조체의 포인터입니다.
 * @return 성공 시 갱신된 스택 포인터(rsp)를 반환하고, 할당 실패 시 NULL을 반환합니다.
 *
 * 이 함수는 if_->rsp를 size만큼 감소시킨 후, 새로 필요해진 페이지가
 * 사용자 영역에 매핑되어 있지 않으면 페이지를 할당하고 매핑합니다.
 * 이후 arg가 가리키는 버퍼에서 size 바이트를 스택으로 복사합니다.
 * 페이지 할당이나 매핑에 실패하면 이미 할당된 페이지를 모두 해제하고
 * rsp를 원래 값으로 복원한 후 NULL을 반환합니다.
 */
static uint64_t *push_stack(char *arg, size_t size, struct intr_frame *if_) {
    ASSERT(size >= 0);
    bool alloc_fail = false;
    uintptr_t old_rsp = if_->rsp;
    if_->rsp = old_rsp - size;

    size_t n = pg_diff(old_rsp, if_->rsp);

    if (old_rsp == USER_STACK) {
        n -= 1;
    }

    uintptr_t page_bottom = pg_round_down(old_rsp);
    for (int i = 0; i < n; i++) {
        page_bottom -= PGSIZE;
        uint8_t *kpage = palloc_get_page(PAL_USER | PAL_ZERO);
        if (kpage != NULL) {
            if (!install_page(page_bottom, kpage, true)) {
                palloc_free_page(kpage);
                page_bottom += PGSIZE;
                alloc_fail = true;
                break;
            }
        }
    }

    if (alloc_fail) {
        for (; page_bottom < pg_round_down(old_rsp); page_bottom += PGSIZE) {
            palloc_free_page(page_bottom);
        }
        if_->rsp = old_rsp;
        return NULL;
    }

    for (char *cur = if_->rsp; cur < old_rsp; cur++) {
        if (arg) {
            *cur = *((char *)arg);
            arg++;
        } else {
            *cur = '\0';
        }
    }
    return if_->rsp;
}

/**
 * @brief 사용자 스택에서 데이터를 팝(pop)합니다.
 *
 * @branch feat/arg-parse
 * @see https://www.notion.so/jactio/arg-parsing-235c9595474e8034af80c8ca37af7dd2?source=copy_link
 * @param size 팝할 바이트 수이며, 0보다 커야 합니다.
 * @param if_ 스택 포인터(rsp)를 조정할 intr_frame 구조체의 포인터입니다.
 * @return 갱신된 스택 포인터(rsp)를 반환합니다.
 *
 * 이 함수는 if_->rsp를 size만큼 증가시킨 후, 더 이상 필요하지
 * 않은 페이지(스택 확장 시 할당된 페이지)를 해제합니다.
 */
static uint64_t *pop_stack(size_t size, struct intr_frame *if_) {
    ASSERT(size > 0);
    ASSERT(if_->rsp + size <= USER_STACK);

    uintptr_t page_bottom = pg_round_down(if_->rsp);
    if_->rsp += size;

    size_t n = ((uintptr_t)pg_round_down(if_->rsp) - page_bottom) >> PGBITS;

    if (if_->rsp == USER_STACK) {
        n -= 1;
    }

    for (int i = 0; i < n; i++) {
        palloc_free_page(page_bottom);
        page_bottom += PGSIZE;
    }

    return if_->rsp;
}

#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool lazy_load_segment(struct page *page, void *aux) {
    struct lazy_segment_arg *lazy_segment = (struct lazy_segment_arg *)aux;
    file_seek(lazy_segment->file, lazy_segment->ofs);
    file_read(lazy_segment->file, page->frame->kva, lazy_segment->ofs);
    memset(page->frame->kva + lazy_segment->read_bytes, 0, lazy_segment->zero_bytes);
    free(aux);

    return true;
}

/* 파일(FILE)의 OFS(offset) 위치부터 시작하여 세그먼트를 
 * 가상 주소 UPAGE에 로드한다. 총 READ_BYTES + ZERO_BYTES 바이트의 
 * 가상 메모리를 다음과 같이 초기화한다:
 *
 * - READ_BYTES 바이트는 FILE의 OFS 위치부터 읽어서 UPAGE에 채운다.
 * - 그 다음 ZERO_BYTES 바이트는 0으로 채운다 (UPAGE + READ_BYTES부터).
 *
 * WRITABLE이 true인 경우, 초기화된 페이지는 사용자 프로세스가 
 * 쓰기 가능해야 하며, false인 경우 읽기 전용이어야 한다.
 *
 * 성공 시 true를 반환하고, 메모리 할당 실패나 디스크 읽기 오류가 
 * 발생하면 false를 반환한다.
 */
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {

    //페이지 정렬
    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0); 
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(ofs % PGSIZE == 0);

    while (read_bytes > 0 || zero_bytes > 0) {
        /* 이 페이지를 어떻게 채울지 계산해야 한다.
         * FILE로부터 PAGE_READ_BYTES만큼 읽고,
         * 나머지 PAGE_ZERO_BYTES는 0으로 채운다. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE; //read_byte가 PGSIZE보다 작으면 그만큼만 읽고, 아니면 딱 한 페이지를 읽는다.
        size_t page_zero_bytes = PGSIZE - page_read_bytes; //남는 부분은 0으로 채워 한 페이지를 꽉 채움

        /* TODO: lazy_load_segment 함수에 정보를 전달하기 위해 aux를 설정하라. */
        struct lazy_segment_arg *aux = malloc(sizeof(struct lazy_segment_arg));
            aux->file = file;
            aux->ofs = ofs;
            aux->read_bytes = read_bytes;
            aux->zero_bytes = zero_bytes;
            aux->writable = writable;

        if (!vm_alloc_page_with_initializer(VM_ANON, upage, writable, lazy_load_segment, aux))
            return false;

        /* Advance. */
        ofs += page_read_bytes;
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
    }
    return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool setup_stack(struct intr_frame *if_) {
    bool success = false;
    void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);

    /* TODO: Map the stack on stack_bottom and claim the page immediately.
     * TODO: If success, set the rsp accordingly.
     * TODO: You should mark the page is stack. */
    /* TODO: Your code goes here */

    return success;
}
#endif /* VM */
