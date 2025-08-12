/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"

#include "threads/malloc.h"
#include "vm/inspect.h"

static struct list frame_table;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void) {
    vm_anon_init();
    vm_file_init();
#ifdef EFILESYS /* For project 4 */
    pagecache_init();
#endif
    register_inspect_intr();
    /* DO NOT MODIFY UPPER LINES. */
    /* TODO: Your code goes here. */

    list_init(&frame_table);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type page_get_type(struct page *page) {
    int ty = VM_TYPE(page->operations->type);
    switch (ty) {
        case VM_UNINIT:
            return VM_TYPE(page->uninit.type);
        default:
            return ty;
    }
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
/*upage가 이미 사용 중인지 확인한다.
페이지를 생성한다.
type에 따라 초기화 함수를 가져온다.
"uninit" 타입의 페이지로 초기화한다.
필드 수정은 uninit_new를 호출한 이후에 해야 한다.
생성한 페이지를 SPT에 추가한다.*/
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
                                    vm_initializer *init, void *aux) {
    ASSERT(VM_TYPE(type) != VM_UNINIT);

    struct supplemental_page_table *spt = &thread_current()->spt;

    typedef bool (*initializer_by_type)(struct page *, enum vm_type, void *);
    initializer_by_type initializer = NULL;

    //1. upage가 spt에 존재하는지 확인
    if (spt_find_page(spt, upage) == NULL) {

        //2. 새로운 페이지 구조체 할당
        struct page *page = malloc(sizeof(struct page));

        if(!page) {
            goto err;
        }

        //3. 페이지 타입에 따라 초기화 함수 가져옴
        switch (VM_TYPE(type)){
            case VM_ANON:
                initializer = anon_initializer;
                break;

            case VM_FILE:
                initializer = file_backed_initializer;
                break;
            
            default:
            return false; //지원하지 않는 경우 false
        }

    //4. 새로운 초기화되지 않은 페이지 생성
    uninit_new(page, upage, init, type, aux, initializer);

    //5. 페이지의 쓰기 가능 여부 설정
    page -> writable = writable;

    //6. 생성한 페이지를 SPT에 삽입
    return spt_insert_page (spt, page);
    }
err:
    return false;
}

//주어진 보조 페이지 테이블에서 va에 해당하는 struct page를 찾아 반환한다.
//찾지 못하면 NULL을 반환한다.
struct page *spt_find_page(struct supplemental_page_table *spt UNUSED, void *va UNUSED) {  
    struct page key;
    key.va = pg_round_down(va); 

    struct hash_elem *e = hash_find (&spt->spt_hash, &key.hash_elem);

    if (e == NULL) {
        return NULL; 
    }
    else {
        return hash_entry (e, struct page, hash_elem);
    }
}

//struct page를 주어진 SPT에 삽입한다.
//단, 해당 page의 가상 주소(page->va)가 SPT에 이미 존재하지 않아야 하며,
//존재할 경우 삽입하지 않고 false를 반환해야 한다.
bool spt_insert_page(struct supplemental_page_table *spt UNUSED, struct page *page UNUSED) {
    page->va = pg_round_down(page->va);
    struct hash_elem *old_elem = hash_insert (&spt->spt_hash, &page->hash_elem);

    if (old_elem != NULL){
        return false;
    }
    return true;
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page) {   
    struct hash_elem *e = hash_delete (&spt->spt_hash, &page->hash_elem);
    if (e == NULL) {
        return false;
    }
    vm_dealloc_page(page);
    return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *vm_get_victim(void) {
    struct frame *victim = NULL;
    /* TODO: The policy for eviction is up to you. */

    return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *vm_evict_frame(void) {
    struct frame *victim UNUSED = vm_get_victim();
    /* TODO: swap out the victim and return the evicted frame. */

    return NULL;
}

//물리 메모리 할당 -> 프레임 구조체 생성 -> 반환
static struct frame *vm_get_frame(void) {
    //물리 페이지 할당
    void *kva = palloc_get_page(PAL_USER);
    if (kva == NULL){
        PANIC("todo"); //swap out 구현 후 수정
    }
    //프레임 구조체 할당
    struct frame *frame = malloc(sizeof(struct frame));
    ASSERT(frame != NULL);

    //멤버 초기화
    frame->kva = kva;
    frame->page = NULL;

    return frame;
}

/* Growing the stack. */
static void vm_stack_growth(void *addr UNUSED) {}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page *page UNUSED) {}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr UNUSED, bool user UNUSED,
                         bool write UNUSED, bool not_present UNUSED) {
    struct supplemental_page_table *spt UNUSED = &thread_current()->spt;
    struct page *page = NULL;
    /* TODO: Validate the fault */
    /* TODO: Your code goes here */

    return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page) {
    destroy(page);
    free(page);
}

//VA에 해당하는 page 찾아서 vm_do_claim_page로 연결
/* Claim the page that allocate on VA. */
bool vm_claim_page(void *va UNUSED) {
    struct page *page = NULL;
    
    page = spt_find_page (&thread_current()->spt,va); //현재 스레드의 SPT에서 va와 매칭되는 페이지 정보를 찾아서 page에 저장
    if (page == NULL){
        return false;
    }

    return vm_do_claim_page(page);
}

/*vm_get_frame()으로 물리 페이지 확보
page와 frame 연결 (frame->page = page, page->frame = frame)
페이지 테이블에 va → kva 매핑 추가 (pml4_set_page)
true/false 반환*/
static bool vm_do_claim_page(struct page *page) {
    struct frame *frame = vm_get_frame(); //물리 페이지 확보

    frame->page = page;
    page->frame = frame;

    if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)) //현재 스레드의 PT에 page->va주소를 frame->kva로 매핑하고 쓰기 가능 여부를 설정
        return false;

    return swap_in(page, frame->kva); //디스크나 파일에서 페이지 내용을 frame에 채움
}

//보조 페이지 테이블을 초기화
/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt UNUSED) {
    hash_init(spt, page_hash, page_less, NULL);
}

// 페이지(가상 주소)에 대한 해시 값을 계산
uint64_t page_hash (const struct hash_elem *e, void *aux) {
    const struct page *p = hash_entry (e,struct page, hash_elem);
    return hash_bytes(&p->va, sizeof p->va);
}

// 두 페이지(가상 주소)를 비교해서 정렬 순서를 결정
bool page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux) {
    const struct page *pa = hash_entry (a, struct page, hash_elem);
    const struct page *pb = hash_entry (a, struct page, hash_elem);

    return pa->va < pb->va;
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED,
                                  struct supplemental_page_table *src UNUSED) {}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED) {
    /* TODO: Destroy all the supplemental_page_table hold by thread and
     * TODO: writeback all the modified contents to the storage. */
}
