// thread.c - Threads
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

/*! @file thread.c
    @brief Thread manager and operations
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/

#ifdef THREAD_TRACE
#define TRACE
#endif

#ifdef THREAD_DEBUG
#define DEBUG
#endif

#include "thread.h"

#include <stddef.h>
#include <stdint.h>

#include "misc.h"
#include "heap.h"
#include "string.h"
#include "riscv.h"
#include "intr.h"
#include "error.h"
#include "see.h"
#include <stdarg.h>
#include "see.h"
// COMPILE-TIME PARAMETERS
//

// NTHR is the maximum number of threads

#ifndef NTHR
#define NTHR 16
#endif

// EXPORTED GLOBAL VARIABLES
//

char thrmgr_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//


enum thread_state {
    THREAD_UNINITIALIZED = 0,
    THREAD_WAITING,
    THREAD_SELF,
    THREAD_READY,
    THREAD_EXITED
};

struct thread_context {
    union {
        uint64_t s[12];
        struct {
            uint64_t a[8];      // s0 .. s7
            void (*pc)(void);   // s8
            uint64_t _pad;      // s9
            void * fp;          // s10
            void * ra;          // s11
        } startup;
    };

    void * ra;
    void * sp;
};

struct thread_stack_anchor {
    struct thread * ktp;
    void * kgp;
};

struct thread {
    struct thread_context ctx;  // must be first member (thrasm.s)
    int id; // index into thrtab[]
    enum thread_state state;
    const char * name;
    struct thread_stack_anchor * stack_anchor;
    void * stack_lowest;
    struct process * proc;
    struct thread * parent;
    struct thread * list_next;
    struct condition * wait_cond;
    struct condition child_exit;
    struct lock * lock_list;
};

// INTERNAL MACRO DEFINITIONS
// 

// Pointer to running thread, which is kept in the tp (x4) register.

#define TP ((struct thread*)__builtin_thread_pointer())

// Macro for changing thread state. If compiled for debugging (DEBUG is
// defined), prints function that changed thread state.

#define set_thread_state(t,s) do { \
    debug("Thread <%s:%d> state changed from %s to %s by <%s:%d> in %s", \
        (t)->name, (t)->id, \
        thread_state_name((t)->state), \
        thread_state_name(s), \
        TP->name, TP->id, \
        __func__); \
    (t)->state = (s); \
} while (0)

// INTERNAL FUNCTION DECLARATIONS
//

// Initializes the main and idle threads. called from threads_init().

static void init_main_thread(void);
static void init_idle_thread(void);

// Sets the RISC-V thread pointer to point to a thread.

static void set_running_thread(struct thread * thr);

// Returns a string representing the state name. Used by debug and trace
// statements, so marked unused to avoid compiler warnings.

static const char * thread_state_name(enum thread_state state)
    __attribute__ ((unused));

// void thread_reclaim(int tid)
//
// Reclaims a thread's slot in thrtab and makes its parent the parent of its
// children. Frees the struct thread of the thread.

static void thread_reclaim(int tid);

// struct thread * create_thread(const char * name)
//
// Creates and initializes a new thread structure. The new thread is not added
// to any list and does not have a valid context (_thread_switch cannot be
// called to switch to the new thread).

static struct thread * create_thread(const char * name);

// void running_thread_suspend(void)
// Suspends the currently running thread and resumes the next thread on the
// ready-to-run list using _thread_swtch (in threasm.s). Must be called with
// interrupts enabled. Returns when the current thread is next scheduled for
// execution. If the current thread is TP, it is marked READY and placed
// on the ready-to-run list. Note that running_thread_suspend will only return if the
// current thread becomes READY.

static void running_thread_suspend(void);

void lock_release_completely(struct lock * lock);

// void release_all_thread_locks(struct thread * thr)
// Releases all locks held by a thread. Called when a thread exits.

static void release_all_thread_locks(struct thread * thr);

// The following functions manipulate a thread list (struct thread_list). Note
// that threads form a linked list via the list_next member of each thread
// structure. Thread lists are used for the ready-to-run list (ready_list) and
// for the list of waiting threads of each condition variable. These functions
// are not interrupt-safe! The caller must disable interrupts before calling any
// thread list function that may modify a list that is used in an ISR.

static void tlclear(struct thread_list * list);
static int tlempty(const struct thread_list * list);
static void tlinsert(struct thread_list * list, struct thread * thr);
static struct thread * tlremove(struct thread_list * list);
static void tlappend(struct thread_list * l0, struct thread_list * l1);

static void idle_thread_func(void);

// IMPORTED FUNCTION DECLARATIONS
// defined in thrasm.s
//

extern struct thread * _thread_swtch(struct thread * thr);

extern void _thread_startup(void);

// INTERNAL GLOBAL VARIABLES
//

#define MAIN_TID 0
#define IDLE_TID (NTHR-1)

static struct thread main_thread;
static struct thread idle_thread;

extern char _main_stack_lowest[]; // from start.s
extern char _main_stack_anchor[]; // from start.s

static struct thread main_thread = {
    .id = MAIN_TID,
    .name = "main",
    .state = THREAD_SELF,
    .stack_anchor = (void*)_main_stack_anchor,
    .stack_lowest = _main_stack_lowest,
    .child_exit.name = "main.child_exit"
};

extern char _idle_stack_lowest[]; // from thrasm.s
extern char _idle_stack_anchor[]; // from thrasm.s

static struct thread idle_thread = {
    .id = IDLE_TID,
    .name = "idle",
    .state = THREAD_READY,
    .parent = &main_thread,
    .stack_anchor = (void*)_idle_stack_anchor,
    .stack_lowest = _idle_stack_lowest,
    .ctx.sp = _idle_stack_anchor,
    .ctx.ra = &_thread_startup,
    // FIXME your code goes here
    // Idle loop executes this function on first run
    //returning from idle tears it dowm cleanly
    //Establish frame pointer at top of idle stack
    .ctx.startup.pc=idle_thread_func,
    .ctx.startup.ra=running_thread_exit,
    .ctx.startup.fp=_idle_stack_anchor
};

static struct thread * thrtab[NTHR] = {
    [MAIN_TID] = &main_thread,
    [IDLE_TID] = &idle_thread
};

static struct thread_list ready_list = {
    .head = &idle_thread,
    .tail = &idle_thread
};

// EXPORTED FUNCTION DEFINITIONS
//


int running_thread(void) {
    return TP->id;
}

void thrmgr_init(void) {
    trace("%s()", __func__);
    init_main_thread();
    init_idle_thread();
    set_running_thread(&main_thread);
    thrmgr_initialized = 1;
}

/*
    Inputs: const char * name -- name for new thread
            void (*entry)(void) --entry point of the child thread
            ... --up to 8 arguments pass to the entry
    Outputs: int --child thread id on success or negative error
    Descriptions: call create thread to create the new thread
                  return error code -EMTHR, which means that the maximum
                  number of threads has been reached, and no new threads can be created.
                  when the new thread is scheduled to execute, it will jump to the entry function provided to
                  thread spawn, passing the additional arguments which were provided
    Side Effects: None
*/
int spawn_thread (
    const char * name,
    void (*entry)(void),
    ...)
{
    struct thread * child;
    va_list ap;
    int pie;
    int i;

    child = create_thread(name);

    if (child == NULL)
        return -EMTHR;

    if(child->name==NULL)
    {
        child->name="thread";
        //provide default name for anonymous threads
    }
    //prepare join condition
    condition_init(&child->child_exit,child->name);
    //init child
    child->ctx.sp=child->stack_anchor; //start with stack pointer at anchor
    //use helper function to setup arguments
    child->ctx.ra=&_thread_startup;
    child->ctx.startup.pc=(entry!=NULL)?entry:(void(*)(void))running_thread_exit;

    //return
    child->ctx.startup.ra=(void*)running_thread_exit;
    child->ctx.startup.fp=child->stack_anchor;
    memset(child->ctx.startup.a,0,sizeof(child->ctx.startup.a));
    //extract the optional arguments  to thread spawn and 
    //save them in the part of the thread context structure normally used to save registers  s0 through s7. 
    va_start(ap, entry);
    for (i = 0; i < 8; i++)
        child->ctx.startup.a[i] = va_arg(ap, uint64_t);
    va_end(ap);
    //set the state to reeady
    set_thread_state(child, THREAD_READY);
    //insert child to ready list
    pie = disable_interrupts();
    //queue child at tail of the ready list
    tlinsert(&ready_list, child);
    restore_interrupts(pie);
    return child->id;
}

/*
    Inputs: None
    Outputs: None
    Descriptions:  first check if the currently running thread is the main thread.
                   If so, halt success() is called. Otherwise, it should set the current thread’s 
                   state to THREAD EXITED.
                   Next, it should signal the parent thread in case it is waiting for the current 
                   thread to exit. Lastly, it should call running thread suspend()
    Side Effects: None
*/
void running_thread_exit(void) {
     // FIXME your code goes here
    struct thread * const thr =TP;
    trace("%s() in<%s:%d>",__func__, thr->name,thr->id);
    if(thr->id==MAIN_TID)
    {
        halt_success();
    }
    release_all_thread_locks(thr);
    //clear the wait condition
    thr->wait_cond=NULL;
    //mark the thread as exited
    int pie=disable_interrupts();
    set_thread_state(thr, THREAD_EXITED);
    restore_interrupts(pie);
    //if parents thread is tracking the child
    if(thr->parent !=NULL)
    {
        //wake any parent waiting on this child_exit condition
        condition_broadcast(&thr->parent->child_exit);
    }
    //context switch away
    running_thread_suspend();

    halt_failure();
}


void running_thread_yield(void) {
    trace("%s() in <%s:%d>", __func__, TP->name, TP->id);
    running_thread_suspend();
}

/*
    Inputs: None
    Outputs: None
    Descriptions:  Child already exited. If the child has already exited, thread join does not need to wait. 
                   It should return immediately. Child still running. 
                   If the child is still running, the parent should wait on the condition variable child exit 
                   in its own struct thread. Note that an existing child signals this condition in thread exit.
                   In either case, the parent should release the resources used by the child thread by calling thread reclaim.
    Side Effects: None
*/
int thread_join(int tid) {
     // FIXME your code goes here
    struct thread * const parent =TP;
    trace("%s() in<%s:%d>",__func__, parent->name,parent->id);
    struct thread * child=NULL;
    int pie=disable_interrupts();
    if(tid!=0){
        if(tid==IDLE_TID){
            restore_interrupts(pie);
            return -EINVAL;
        }
        //no child and parent relationship
        if(!(0<tid && tid<NTHR)||thrtab[tid]==NULL||thrtab[tid]->parent!=parent){
            restore_interrupts(pie);
            return -EINVAL;
        }
        //Wait until THREAD_EXITED
        child=thrtab[tid];
        while(child->state!=THREAD_EXITED){
            //sleep until child broadcast exit
            condition_wait(&parent->child_exit);
        }
    }
    else{
        while(1){
            int found_child=0;
            child=NULL;
            //scan all threads look for children
            for(int i=1;i<NTHR;i++){
                struct thread * thrchild =thrtab[i];
                if(thrchild==NULL||thrchild->parent!=parent||thrchild->id==IDLE_TID){
                    //if parent not the parent we find
                    continue;
                }
                found_child=1;
                if(thrchild->state==THREAD_EXITED){
                    child=thrchild;
                    break;
                }
            }
            //if found cliam
            if(child!=NULL){
                break;
            }
            //no child return ENIVAL
            if(!found_child){
                restore_interrupts(pie);
                return -EINVAL;
            }
            //wait until any child finishes
            condition_wait(&parent->child_exit);
        }
    }
    assert(child!=NULL);
    int cid=child->id;
    restore_interrupts(pie);
    //reclaim the  child resource
    thread_reclaim(cid);
    return cid;
}

struct process * thread_process(int tid) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    return thrtab[tid]->proc;
}

struct process * running_thread_process(void) {
    return TP->proc;
}

void thread_set_process(int tid, struct process * proc) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    thrtab[tid]->proc = proc;
}

void thread_detach(int tid) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    thrtab[tid]->parent = NULL;
}

const char * thread_name(int tid) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    return thrtab[tid]->name;
}

const char * running_thread_name(void) {
    return TP->name;
}

void * running_thread_stack_base(void){
    return TP->stack_anchor;
}

void condition_init(struct condition * cond, const char * name) {
    tlclear(&cond->wait_list);
    cond->name = name;
}

void condition_wait(struct condition * cond) {
    int pie;

    trace("%s(cond=<%s>) in <%s:%d>", __func__,
        cond->name, TP->name, TP->id);

    assert(TP->state == THREAD_SELF);

    // Insert current thread into condition wait list
    
    set_thread_state(TP, THREAD_WAITING);
    TP->wait_cond = cond;
    TP->list_next = NULL;

    pie = disable_interrupts();
    tlinsert(&cond->wait_list, TP);
    restore_interrupts(pie);

    running_thread_suspend();
}

/*
    Inputs: struct condition * cond -- condition variable 
    Outputs: None
    Descriptions:  Changing its state from THREAD WAITING to THREAD READY,
                   placing it on the ready list, and
                   removing it from the list of threads waiting on the condition.
    Side Effects: None
*/
void condition_broadcast(struct condition * cond) {
    // FIXME your code goes here
    struct thread * thr;
    int pie;
    trace("%s(cond=<%s>)",__func__,cond->name);
    //temporary list for ready threads
    struct thread_list wake={0};
    pie=disable_interrupts();
    //get the first list pointer
    //pop first waiting thread
    thr=tlremove(&cond->wait_list);
    while(thr!=NULL)
    {
        //empty all the thread in wait
        thr->wait_cond=NULL;
        // set the state to ready
        set_thread_state(thr,THREAD_READY);
        //put it in to the ready list
        tlinsert(&wake, thr);
        //get the next
        thr=tlremove(&cond->wait_list);
    }
    //append all woken threads to ready queue
    tlappend(&ready_list, &wake);
    restore_interrupts(pie);
}

void lock_init(struct lock * lock) {
    memset(lock, 0, sizeof(struct lock));
    condition_init(&lock->release, "lock_release");
}

void lock_acquire(struct lock * lock) {
    if (lock->owner != TP) {
        while (lock->owner != NULL)
            condition_wait(&lock->release);
        
        lock->owner = TP;
        lock->cnt = 1;
        lock->next = TP->lock_list;
        TP->lock_list = lock;
    } else
        lock->cnt += 1;
}

void lock_release(struct lock * lock) {
    assert (lock->owner == TP);
    assert (lock->cnt != 0);

    lock->cnt -= 1;

    if (lock->cnt == 0)
        lock_release_completely(lock);
}

// INTERNAL FUNCTION DEFINITIONS
//

void init_main_thread(void) {
    // Initialize stack anchor with pointer to self
    main_thread.stack_anchor->ktp = &main_thread;
}

void init_idle_thread(void) {
    // Initialize stack anchor with pointer to self
    idle_thread.stack_anchor->ktp = &idle_thread;
}

static void set_running_thread(struct thread * thr) {
    asm inline ("mv tp, %0" :: "r"(thr) : "tp");
}

const char * thread_state_name(enum thread_state state) {
    static const char * const names[] = {
        [THREAD_UNINITIALIZED] = "UNINITIALIZED",
        [THREAD_WAITING] = "WAITING",
        [THREAD_SELF] = "SELF",
        [THREAD_READY] = "READY",
        [THREAD_EXITED] = "EXITED"
    };

    if (0 <= (int)state && (int)state < sizeof(names)/sizeof(names[0]))
        return names[state];
    else
        return "UNDEFINED";
};

void thread_reclaim(int tid) {
    struct thread * const thr = thrtab[tid];
    int ctid;

    assert (0 < tid && tid < NTHR && thr != NULL);
    assert (thr->state == THREAD_EXITED);

    // Make our parent thread the parent of our child threads. We need to scan
    // all threads to find our children. We could keep a list of all of a
    // thread's children to make this operation more efficient.

    for (ctid = 1; ctid < NTHR; ctid++) {
        if (thrtab[ctid] != NULL && thrtab[ctid]->parent == thr)
            thrtab[ctid]->parent = thr->parent;
    }

    thrtab[tid] = NULL;
    kfree(thr);
}

struct thread * create_thread(const char * name) {
    struct thread_stack_anchor * anchor;
    void * stack_lowest;
    size_t stack_size;
    struct thread * thr;
    int tid;

    trace("%s(name=\"%s\") in <%s:%d>", __func__, name, TP->name, TP->id);

    // Find a free thread slot.

    tid = 0;
    while (++tid < NTHR)
        if (thrtab[tid] == NULL)
            break;
    
    if (tid == NTHR)
        return NULL;
    
    // Allocate a struct thread and a stack

    thr = kcalloc(1, sizeof(struct thread));
    
    stack_size = 4000; // change to PAGE_SIZE in mp3
    stack_lowest = kmalloc(stack_size);
    anchor = stack_lowest + stack_size;
    anchor -= 1; // anchor is at base of stack
    thr->stack_lowest = stack_lowest;
    thr->stack_anchor = anchor;
    anchor->ktp = thr;
    anchor->kgp = NULL;

    thrtab[tid] = thr;

    thr->id = tid;
    thr->name = name;
    thr->parent = TP;
    thr->proc = TP->proc;
    return thr;
}

/*
    Inputs: None
    Outputs: None
    Descriptions:  The thread being suspended, if it is still runnable, is inserted at the tail of ready list, 
                   and the next thread to run is taken from the head of the list. 
                   The calling thread may be THREAD RUNNING, THREAD WAITING, or THREAD EXITED. 
                   place the current thread back on the ready-to-run list 
                   check if the thread that called it is in the THREAD EXITED state. 
                   if so, free its stack
    Side Effects: None
*/
void running_thread_suspend(void) {
    // FIXME your code goes here
    struct thread * const current=TP;
    struct thread * next;
    struct thread * prev;
    int pie=disable_interrupts();

    if(current->state==THREAD_SELF)
    {
        //mark it as ready and enqueue it and the tail of the ready list
        set_thread_state(current, THREAD_READY);
        tlinsert(&ready_list, current);
    }
    // else
    // {
    //     assert(current->state==THREAD_WAITING||current->state==THREAD_EXITED);
    // }
    // Pick the next runnable thread
    next=tlremove(&ready_list);
    //assert(next!=NULL);
    //mark new runing thread
    set_thread_state(next, THREAD_SELF);
    //keep interrupt before go to the switch
    enable_interrupts();
    //swap context
    prev=_thread_swtch(next);
    restore_interrupts(pie);
    //if exited free it memory
    if(prev!=NULL&& prev->state==THREAD_EXITED&& prev->stack_lowest!=NULL)
    {
        kfree(prev->stack_lowest);
        prev->stack_anchor=NULL;
        prev->stack_lowest=NULL;

    }
}

void tlclear(struct thread_list * list) {
    list->head = NULL;
    list->tail = NULL;
}

int tlempty(const struct thread_list * list) {
    return (list->head == NULL);
}

void tlinsert(struct thread_list * list, struct thread * thr) {
    thr->list_next = NULL;

    if (thr == NULL)
        return;

    if (list->tail != NULL) {
        assert (list->head != NULL);
        list->tail->list_next = thr;
    } else {
        assert(list->head == NULL);
        list->head = thr;
    }

    list->tail = thr;
}

struct thread * tlremove(struct thread_list * list) {
    struct thread * thr;

    thr = list->head;
    
    if (thr == NULL)
        return NULL;

    list->head = thr->list_next;
    
    if (list->head != NULL)
        thr->list_next = NULL;
    else
        list->tail = NULL;

    thr->list_next = NULL;
    return thr;
}

void tlappend(struct thread_list * l0, struct thread_list * l1) {
    if (l0->head != NULL) {
        assert(l0->tail != NULL);
        
        if (l1->head != NULL) {
            assert(l1->tail != NULL);
            l0->tail->list_next = l1->head;
            l0->tail = l1->tail;
        }
    } else {
        assert(l0->tail == NULL);
        l0->head = l1->head;
        l0->tail = l1->tail;
    }

    l1->head = NULL;
    l1->tail = NULL;
}

void lock_release_completely(struct lock * lock) {
    struct lock ** hptr;

    condition_broadcast(&lock->release);
    hptr = &TP->lock_list;
    while (*hptr != lock && *hptr != NULL)
        hptr = &(*hptr)->next;
    assert (*hptr != NULL);
    *hptr = (*hptr)->next;
    lock->owner = NULL;
    lock->next = NULL;
}

void release_all_thread_locks(struct thread * thr) {
    struct lock * head;
    struct lock * next;

    head = thr->lock_list;

    while (head != NULL) {
        next = head->next;
        head->next = NULL;
        head->owner = NULL;
        head->cnt = 0;
        condition_broadcast(&head->release);
        head = next;
    }

    thr->lock_list = NULL;
}

void idle_thread_func(void) {
    // The idle thread sleeps using wfi if the ready list is empty. Note that we
    // need to disable interrupts before checking if the thread list is empty to
    // avoid a race condition where an ISR marks a thread ready to run between
    // the call to tlempty() and the wfi instruction.

    for (;;) {
        // If there are runnable threads, yield to them.

        while (!tlempty(&ready_list))
            running_thread_yield();
        
        // No runnable threads. Sleep using the wfi instruction. Note that we
        // need to disable interrupts and check the runnable thread list one
        // more time (make sure it is empty) to avoid a race condition where an
        // ISR marks a thread ready before we call the wfi instruction.

        disable_interrupts();
        if (tlempty(&ready_list))
            asm ("wfi");
        enable_interrupts();
    }
}