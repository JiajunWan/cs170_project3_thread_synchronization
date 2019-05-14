/**
 * Sean Gillen, Ryan Allen May 2019
 *
 * This file will automatically grade your cs170 threads library
 * Please see the README for more info
 */


#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#define NUM_TESTS 8
#define PASS 1
#define FAIL 0

//#define _DEBUG
#define TEST_WAIT_MILI 2000 // how many miliseconds do we wait before assuming a test is hung

void lock();
void unlock();

//if your code compiles you pass test 0 for free
//==============================================================================
static int test0(void){
    return PASS;
}


//basic errorno test
//==============================================================================
static void* _thread_errno_test(void* arg){
    pthread_exit(0);
}

static int test1(void){
    pthread_t tid1;
        
    pthread_create(&tid1, NULL,  &_thread_errno_test, NULL);
    
    pthread_join(tid1, NULL);
    sleep(1);
    int rtn = pthread_join(tid1, NULL);
    if (rtn != ESRCH)
        return FAIL;
    return PASS;
}


//basic pthread join test
//==============================================================================
static void* _thread_join_test(void* arg){
    sleep(1);
    pthread_exit(0);
}

static int test2(void){
    pthread_t tid1 = 0;
    
    pthread_create(&tid1, NULL,  &_thread_join_test, NULL);
    
    int rtn = pthread_join(tid1, NULL); //failure occurs on a timeout
    if (rtn != 0)
        return FAIL;
    return PASS;
}


//join test 2
//==============================================================================
static int a41;
static int a42;
static void* _thread_slow_join(void* arg){
    sleep(1);
    a42 = 12;
    pthread_exit(0);
}
static void* _thread_fast_join(void* arg){
    a41 = 11;
    pthread_exit(0);
}

static int test3(void){
    pthread_t tid1; pthread_t tid2;
    void *rtn1, *rtn2;
        
    pthread_create(&tid1, NULL,  &_thread_fast_join, NULL);
    pthread_create(&tid2, NULL,  &_thread_slow_join, NULL);
   
    pthread_join(tid2, &rtn2);
    pthread_join(tid1, &rtn1);
    
    if (a41 != 11 || a42 != 12)
        return FAIL;
    return PASS;
}


//join test 3
//==============================================================================
static void* _thread_dummy_loop(void* arg){
    pthread_t a = pthread_self();
    int *val;
    for(int i = 0; i < 100000; i++);
    
    val  = (int *) malloc(sizeof(int));
    *val = 42;

    return (void *) val;
}

static int test4(void){
    pthread_t tid;
    void *val;
    int i;
    int tids[128];
    
    for (i=0; i<128; i++) {
        pthread_create(&tid, NULL, &_thread_dummy_loop, NULL);
        tids[i] = tid;
    }

    for (i=0; i<128; i++) {
        pthread_join(tids[i], &val);
        if (*(int*)val != 42)
            return FAIL;
    }

    return PASS;
}


//basic semaphore test
//==============================================================================
static int a5;
static void* _thread_arg(void* arg){
    sem_t *sem = (sem_t*) arg;

    sem_wait(sem);
    a5 = 20;
    sem_post(sem);
    
    pthread_exit(0);
}

static int test5(void){
    pthread_t tid1;
    sem_t sem;
    int i;

    sem_init(&sem, 0, 1);
    a5 = 10;

    sem_wait(&sem);
    pthread_create(&tid1, NULL,  &_thread_arg, &sem);

    for (i=0; i<3; i++) {  // sleep for 3 schedules
        sleep(1);
    }
    
    if (a5 != 10)
        return FAIL;
    sem_post(&sem);

    pthread_join(tid1, NULL);
    sem_destroy(&sem);

    return PASS;
}


//semaphore test 2
//==============================================================================
static int a6;
static void* _thread_sem2(void* arg){
    sem_t *sem;
    int local_var, i;

    sem = (sem_t*) arg;

    sem_wait(sem);
    local_var = a6;
    for (i=0; i<5; i++) {
        local_var += pthread_self();
    }
    for (i=0; i<3; i++) {   // sleep for 3 schedules
        sleep(1);
    }
    a6 = local_var;

    sem_post(sem);
    pthread_exit(0);
}

static int test6(void){
    pthread_t tid1, tid2;
    sem_t sem;

    sem_init(&sem, 0, 1);
    a6 = 10;

    pthread_create(&tid1, NULL, &_thread_sem2, &sem);
    pthread_create(&tid2, NULL, &_thread_sem2, &sem);

    pthread_join(tid1, NULL);
    pthread_join(tid2, NULL);

    sem_destroy(&sem);

    if (a6 != (10 + (tid1*5) + (tid2*5)))
        return FAIL;
    return PASS;

}

//basic lock test
//==============================================================================
static int l;
static void* _thread_lock_test(void* arg){
    lock();
    l = 20;
    unlock();

    pthread_exit(0);
}

static int test7(void){
    pthread_t tid;
    int i;

    pthread_create(&tid, NULL,  &_thread_lock_test, NULL);

    lock(); // need to lock after init is called
    l = 10;
    sleep(1);

    if (l != 10)
        return FAIL;
    unlock();
    
    pthread_join(tid, NULL);

    if (l != 20)
        return FAIL;
    return PASS;
}



//end of tests
//==============================================================================


/**
 *  Some implementation details: Main spawns a child process for each
 *  test, that way if test 2/20 segfaults, we can still run the remaining
 *  tests. It also hands the child a pipe to write the result of the test.
 *  the parent polls this pipe, and counts the test as a failure if there
 *  is a timeout (which would indicate the child is hung).
 */


static int (*test_arr[NUM_TESTS])(void) = {&test0, &test1, &test2, &test3, &test4, &test5, &test6, &test7};


int main(void){
    
    int status; pid_t pid;
    int pipe_fd[2]; int timeout; struct pollfd poll_fds;
    int score = 0; int total_score = 0;

    int devnull_fd = open("/dev/null", O_WRONLY);

    pipe(pipe_fd);
    poll_fds.fd = pipe_fd[0]; // only going to poll the read end of our pipe
    poll_fds.events = POLLRDNORM; //only care about normal read operations
        
    for(int i = 0; i < NUM_TESTS; i++){
        score = 0;
        pid = fork();

        //child, launches the test
        if (pid == 0){
#ifndef _DEBUG
            dup2(devnull_fd, STDOUT_FILENO); //begone debug messages
            dup2(devnull_fd, STDERR_FILENO);
#endif
            
            score = test_arr[i]();
            
            write(pipe_fd[1], &score, sizeof(score));
            exit(0); 
        }

        //parent, polls on the pipe we gave the child, kills the child,
        //keeps track of score
        else{  
            
            if(poll(&poll_fds, 1, TEST_WAIT_MILI)){
                read(pipe_fd[0], &score, sizeof(score));
            }
            
            total_score += score;
            kill(pid, SIGKILL);
            waitpid(pid,&status,0);
            
            
            if(score){
                printf("test %i : PASS\n", i);
            }
            else{
                printf("test %i : FAIL\n", i);
            }
        }
    }
    
    printf("total score was %i / %i\n", total_score, NUM_TESTS);
    return 0;
}
