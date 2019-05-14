#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <setjmp.h>
#include <time.h>

#define MAX_THREADS 128
#define MAX_QUEUE 129
#define INTERVAL 50

enum STATE
{
    ACTIVE,
    READY,
    EXITED
};

struct ThreadControlBlock
{
    pthread_t ThreadID;
    unsigned long *ESP;
    enum STATE Status;
    void *(*start_routine)(void *);
    void *arg;
    jmp_buf Registers;
};

struct TCBPool
{
    /* Maximum 129 concurrent running threads including main thread whose TCB Index is 0 */
    struct ThreadControlBlock TCB[MAX_THREADS + 1];
};

struct queue
{
    int front;     /* Init 0 */
    int rear;      /* Init -1 */
    int itemCount; /* Init 0 */
    int IndexQueue[MAX_QUEUE];
};

int peekfront(struct queue *Queue);

int size(struct queue *Queue);

void pushback(struct queue *Queue, int Index);

void pushsecond(struct queue *Queue, int Index);

int popfront(struct queue *Queue);

void Scheduler();

void WrapperFunction();

static long int i64_ptr_mangle(long int p);

struct queue SchedulerThreadPoolIndexQueue = {0, -1, 0};

struct TCBPool ThreadPool;

static int Initialized = 0;

static int NextCreateTCBIndex = 1;

static unsigned long ThreadID = 1;

static struct sigaction sigact;
static struct itimerval Timer;
static struct itimerval Zero_Timer = {0};

void WrapperFunction()
{
    int TCBIndex = peekfront(&SchedulerThreadPoolIndexQueue);
    ThreadPool.TCB[TCBIndex].start_routine(ThreadPool.TCB[TCBIndex].arg);
    pthread_exit(0);
}

void main_thread_init()
{
    int i;
    for (i = 1; i < MAX_THREADS + 1; i++)
    {
        ThreadPool.TCB[i].Status = EXITED;
    }
    Initialized = 1;

    /* Main thread TCB Index is 0 and Main thread ID is 99999 */
    ThreadPool.TCB[0].ThreadID = 99999;
    ThreadPool.TCB[0].Status = ACTIVE;
    ThreadPool.TCB[0].ESP = NULL;
    ThreadPool.TCB[0].start_routine = NULL;
    ThreadPool.TCB[0].arg = NULL;
    setjmp(ThreadPool.TCB[0].Registers);
    pushback(&SchedulerThreadPoolIndexQueue, 0);

    sigact.sa_handler = Scheduler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = SA_NODEFER;
    if (sigaction(SIGALRM, &sigact, NULL) == -1)
    {
        perror("Unable to catch SIGALRM!");
        exit(1);
    }
    Timer.it_value.tv_sec = INTERVAL / 1000;
    Timer.it_value.tv_usec = (INTERVAL * 1000) % 1000000;
    Timer.it_interval = Timer.it_value;

    if (setitimer(ITIMER_REAL, &Timer, NULL) == -1)
    {
        perror("Error calling setitimer()");
        exit(1);
    }
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
    /* If not initialized, initialize thread pool */
    if (!Initialized)
    {
        /* Initialize the main thread pool */
        main_thread_init();
    }
    if (size(&SchedulerThreadPoolIndexQueue) < MAX_QUEUE)
    {

        /* Pause Timer */
        setitimer(ITIMER_REAL, &Zero_Timer, &Timer);

        /* Find the next available Thread Pool slot */
        int i;
        for (i = 1; i < MAX_QUEUE; i++)
        {
            if (ThreadPool.TCB[i].Status == EXITED)
            {
                NextCreateTCBIndex = i;
                break;
            }
        }

        /* Initialize for the chosen slot */
        ThreadPool.TCB[NextCreateTCBIndex].ThreadID = ThreadID++;
        ThreadPool.TCB[NextCreateTCBIndex].ESP = (unsigned long *)malloc(32767);
        ThreadPool.TCB[NextCreateTCBIndex].Status = ACTIVE;
        ThreadPool.TCB[NextCreateTCBIndex].start_routine = start_routine;
        ThreadPool.TCB[NextCreateTCBIndex].arg = arg;
        *thread = ThreadPool.TCB[NextCreateTCBIndex].ThreadID;

        /* Setjmp */
        setjmp(ThreadPool.TCB[NextCreateTCBIndex].Registers);

        /* Save the address of Wrapper Function to a pointer */
        void (*WrapperFunctionPointer)() = &WrapperFunction;

        /* Change External Stack Pointer in the jmp_buf */
        ThreadPool.TCB[NextCreateTCBIndex].Registers[0].__jmpbuf[6] = i64_ptr_mangle((unsigned long)(ThreadPool.TCB[NextCreateTCBIndex].ESP + 32759 / 8 - 2));

        /* Change External Instruction Pointer to Wrapper Function in the jmp_buf */
        ThreadPool.TCB[NextCreateTCBIndex].Registers[0].__jmpbuf[7] = i64_ptr_mangle((unsigned long)WrapperFunctionPointer);

        /* Add the New Thread Thread Pool Index to the second place in the Queue in case of segfault when freeing esp of last thread with high index*/
        pushsecond(&SchedulerThreadPoolIndexQueue, NextCreateTCBIndex);

        /* Resume Timer */
        setitimer(ITIMER_REAL, &Timer, NULL);

        return 0;
    }

    /* Reach the Max number of concurrent threads and return -1 as error */
    else
    {
        return -1;
    }
}

pthread_t pthread_self(void)
{
    return (pthread_t)ThreadPool.TCB[peekfront(&SchedulerThreadPoolIndexQueue)].ThreadID;
}

void pthread_exit(void *value_ptr)
{
    /* If no pthread_create call, only main */
    if (Initialized == 0)
    {
        exit(0);
    }

    int Index = popfront(&SchedulerThreadPoolIndexQueue);
    /* Stop Timer */
    setitimer(ITIMER_REAL, &Zero_Timer, NULL);

    /* If exit last thread, first free, and then exit */
    if (size(&SchedulerThreadPoolIndexQueue) == 0)
    {
        if (Index != 0)
        {
            free((unsigned long *)ThreadPool.TCB[Index].ESP);
        }
        exit(0);
    }

    /* Clean Up */
    if (Index != 0)
    {
        free((unsigned long *)ThreadPool.TCB[Index].ESP);
    }
    ThreadPool.TCB[Index].ThreadID = 0;
    ThreadPool.TCB[Index].ESP = NULL;
    ThreadPool.TCB[Index].start_routine = NULL;
    ThreadPool.TCB[Index].arg = NULL;
    ThreadPool.TCB[Index].Status = EXITED;

    /* Start Timer */
    setitimer(ITIMER_REAL, &Timer, NULL);

    /* Longjmp to the front(next) thread registers */
    longjmp(ThreadPool.TCB[peekfront(&SchedulerThreadPoolIndexQueue)].Registers, 1);
}

void Scheduler()
{
    /* If only one main thread, just return */
    if (size(&SchedulerThreadPoolIndexQueue) <= 1)
    {
        return;
    }

    if (setjmp(ThreadPool.TCB[peekfront(&SchedulerThreadPoolIndexQueue)].Registers) == 0)
    {
        /* Pushback the poped front Thread Pool Index of the saved thread to the end of queue */
        int Index = peekfront(&SchedulerThreadPoolIndexQueue);
        pushback(&SchedulerThreadPoolIndexQueue, Index);
        popfront(&SchedulerThreadPoolIndexQueue);

        /* Longjmp to the front(next) thread registers */
        longjmp(ThreadPool.TCB[peekfront(&SchedulerThreadPoolIndexQueue)].Registers, 1);
    }
    return;
}

int peekfront(struct queue *Queue)
{
    return Queue->IndexQueue[Queue->front];
}

int size(struct queue *Queue)
{
    return Queue->itemCount;
}

void pushback(struct queue *Queue, int Index)
{

    if (Queue->rear == MAX_QUEUE - 1)
    {
        Queue->rear = -1;
    }

    Queue->rear += 1;
    Queue->IndexQueue[Queue->rear] = Index;
    Queue->itemCount += 1;
}

void pushsecond(struct queue *Queue, int Index)
{

    if (Queue->rear == MAX_QUEUE - 1)
    {
        Queue->rear = -1;
    }

    Queue->rear += 1;
    int i;
    for (i = Queue->rear; i > Queue->front + 1; i--)
    {
        Queue->IndexQueue[i] = Queue->IndexQueue[i-1];
    }
    Queue->IndexQueue[Queue->front + 1] = Index;
    Queue->itemCount += 1;
}

int popfront(struct queue *Queue)
{
    int Index = Queue->IndexQueue[Queue->front];
    Queue->front += 1;

    if (Queue->front == MAX_QUEUE)
    {
        Queue->front = 0;
    }

    Queue->itemCount -= 1;
    return Index;
}

static long int i64_ptr_mangle(long int p)
{
    long int ret;
    asm(" mov %1, %%rax;\n"
        " xor %%fs:0x30, %%rax;"
        " rol $0x11, %%rax;"
        " mov %%rax, %0;"
        : "=r"(ret)
        : "r"(p)
        : "%rax");
    return ret;
}
