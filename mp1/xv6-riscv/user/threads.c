#include "kernel/types.h"
#include "user/setjmp.h"
#include "user/threads.h"
#include "user/user.h"
#define NULL 0


static struct thread* current_thread = NULL;
static struct thread* exit_thread = NULL;
static int id = 1;
static jmp_buf env_st;
static jmp_buf env_tmp;

struct thread *thread_create(void (*f)(void *), void *arg){
    struct thread *t = (struct thread*) malloc(sizeof(struct thread));
    //unsigned long stack_p = 0;
    unsigned long new_stack_p;
    unsigned long new_stack;
    new_stack = (unsigned long) malloc(sizeof(unsigned long)*0x100);
    new_stack_p = new_stack +0x100*8-0x2*8;
    t->fp = f;
    t->arg = arg;
    t->ID  = id;
    t->buf_set = 0;
    t->stack = (void*) new_stack;
    t->stack_p = (void*) new_stack_p;
    id++;
    return t;
}
void thread_add_runqueue(struct thread *t){
    if(current_thread == NULL){
        // TODO
        current_thread = t;

        t->previous = t;
        t->next = t;
    }
    else{
        // TODO
        t->previous = current_thread->previous;
        t->next = current_thread;

        current_thread->previous->next = t;
        current_thread->previous = t;
    }
}
void thread_yield(void){
    // TODO
    int ret;

    ret = setjmp(current_thread->env);
    if (ret == 0) {
        /* current thread calling setjmp() to store context */
        if (current_thread->buf_set == 1) {
            /* shit happens... */
        }

        current_thread->buf_set = 1;
        schedule();
        dispatch();
    } else {
        /* other thread calling longjmp() to restore context */
        current_thread->buf_set = 0;

        /* free the exit thread */
        if (exit_thread != NULL) {
            free(exit_thread->stack);
            free(exit_thread);

            exit_thread = NULL;
        }
    }
}
void thread_worker(void *arg) {
    /* free the exit thread */
    if (exit_thread != NULL) {
        free(exit_thread->stack);
        free(exit_thread);

        exit_thread = NULL;
    }

    /* thread stack is ready; running the thread function */
    current_thread->fp(current_thread->arg);

    /* in case returning from thread function */
    thread_exit();
}
void dispatch(void){
    // TODO
    if (current_thread->buf_set == 0) {
        /* first time running; jump to thread_worker() with thread stack */
        env_tmp->ra = (unsigned long)thread_worker;
        env_tmp->sp = (unsigned long)current_thread->stack_p;

        longjmp(env_tmp, 0);
    } else {
        /* there is valid context; restore the context directly */
        longjmp(current_thread->env, 0);
    }
}
void schedule(void){
    current_thread = current_thread->next;
}
void thread_exit(void){
    if(current_thread->next != current_thread){
        // TODO
        current_thread->previous->next = current_thread->next;
        current_thread->next->previous = current_thread->previous;

        /* need to be freed after the stack switching */
        exit_thread = current_thread;

        schedule();
        dispatch();
    }
    else{
        // TODO
        // Hint: No more thread to execute
        longjmp(env_st, 0);
    }
}
void thread_start_threading(void){
    // TODO
    int ret;

    if (!current_thread) {
        fprintf(2, "thread: no thread to start\n");
        return;
    }

    ret = setjmp(env_st);
    if (ret == 0) {
        dispatch();
    } else {
        /* return from thread_exit() */
        if (current_thread != NULL) {
            free(current_thread->stack);
            free(current_thread);

            current_thread = NULL;
        }
    }
}
