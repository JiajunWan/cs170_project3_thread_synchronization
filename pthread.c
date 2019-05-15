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
#include <semaphore.h>

#define SEM_VALUE_MAX 65536
#define MAX_SEMAPHORE 128
#define MAX_THREADS 128
#define MAX_QUEUE 129
#define INTERVAL 50
#define ESRCH 3

enum STATE
{
    ACTIVE,
    BLOCKED,
    EXITED,
    REAPED
};

struct ThreadControlBlock
{
    pthread_t ThreadID;
    unsigned long *ESP;
    enum STATE Status;
    void *(*start_routine)(void *);
    void *arg;
    jmp_buf Registers;
    int CallingThreadTCBIndex;
    int JustCreated;
    void *ReturnValue;
};

struct TCBPool
{
    /* Maximum 129 concurrent running threads including main thread whose TCB Index is 0 */
    struct ThreadControlBlock TCB[MAX_THREADS + 1];
};

struct Semaphore
{
    unsigned int ID;
    unsigned int Value;
    int semaphore_max;
    int Initialized;
};

struct SemaphorePool
{
    struct Semaphore semaphore[MAX_SEMAPHORE];
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

void lock();

void unlock();

static long int i64_ptr_mangle(long int p);

struct queue SchedulerThreadPoolIndexQueue = {0, -1, 0};

struct queue Queue[MAX_SEMAPHORE];

struct TCBPool ThreadPool;

struct SemaphorePool SemPool;

static int Initialized = 0;

static int NextCreateTCBIndex = 1;

static unsigned long ThreadID = 1;

static int SemaphoreID = 0;

static int SemaphoreCount = 0;

static struct sigaction sigact;
static struct itimerval Timer;
static struct itimerval Zero_Timer = {0};
static sigset_t mask;

void WrapperFunction()
{
    int TCBIndex = peekfront(&SchedulerThreadPoolIndexQueue);
    void *ReturnValue = ThreadPool.TCB[TCBIndex].start_routine(ThreadPool.TCB[TCBIndex].arg);
    pthread_exit(ReturnValue);
}

void main_thread_init()
{
    int i;
    for (i = 1; i < MAX_THREADS + 1; i++)
    {
        ThreadPool.TCB[i].Status = REAPED;
    }
    Initialized = 1;

    /* Main thread TCB Index is 0 and Main thread ID is 99999 */
    ThreadPool.TCB[0].ThreadID = 99999;
    ThreadPool.TCB[0].Status = ACTIVE;
    ThreadPool.TCB[0].ESP = NULL;
    ThreadPool.TCB[0].start_routine = NULL;
    ThreadPool.TCB[0].arg = NULL;
    ThreadPool.TCB[0].CallingThreadTCBIndex = -1;
    ThreadPool.TCB[0].JustCreated = 1;
    ThreadPool.TCB[0].ReturnValue = NULL;
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
    setjmp(ThreadPool.TCB[0].Registers);
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
            if (ThreadPool.TCB[i].Status == REAPED)
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
        ThreadPool.TCB[NextCreateTCBIndex].CallingThreadTCBIndex = -1;
        ThreadPool.TCB[NextCreateTCBIndex].JustCreated = 1;
        ThreadPool.TCB[NextCreateTCBIndex].ReturnValue = NULL;
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
        pushback(&SchedulerThreadPoolIndexQueue, NextCreateTCBIndex);

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

    lock();

    int Index = popfront(&SchedulerThreadPoolIndexQueue);

    /* If exit last thread, first free, and then exit */
    if (size(&SchedulerThreadPoolIndexQueue) == 0)
    {
        if (Index != 0)
        {
            free((unsigned long *)ThreadPool.TCB[Index].ESP);
        }
        unlock();
        exit(0);
    }

    /* Clean Up */
    if (Index != 0)
    {
        free((unsigned long *)ThreadPool.TCB[Index].ESP);
    }
    ThreadPool.TCB[Index].ESP = NULL;
    ThreadPool.TCB[Index].start_routine = NULL;
    ThreadPool.TCB[Index].arg = NULL;
    ThreadPool.TCB[Index].Status = EXITED;
    ThreadPool.TCB[Index].ReturnValue = value_ptr;
    if (ThreadPool.TCB[Index].CallingThreadTCBIndex != -1)
    {
        ThreadPool.TCB[ThreadPool.TCB[Index].CallingThreadTCBIndex].Status = ACTIVE;
    }
    pushback(&SchedulerThreadPoolIndexQueue, Index);

    /* Longjmp to the front(next) thread registers */
    Index = peekfront(&SchedulerThreadPoolIndexQueue);
    while (ThreadPool.TCB[Index].Status == BLOCKED || ThreadPool.TCB[Index].Status == EXITED || ThreadPool.TCB[Index].Status == REAPED)
    {
        /* If the thread is not reaped, then pushback the front Thread Pool Index of the saved thread to the end of queue */
        if (ThreadPool.TCB[Index].Status != REAPED)
        {
            pushback(&SchedulerThreadPoolIndexQueue, Index);
        }
        popfront(&SchedulerThreadPoolIndexQueue);
        Index = peekfront(&SchedulerThreadPoolIndexQueue);
    }

    unlock();

    longjmp(ThreadPool.TCB[Index].Registers, 1);
}

int pthread_join(pthread_t thread, void **value_ptr)
{
    lock();
    int i = 0;
    int found = 0;
    int Index = 0;
    int TargetThreadTCBIndex = 0;
    int CallingThreadTCBIndex = peekfront(&SchedulerThreadPoolIndexQueue);
    while (i < SchedulerThreadPoolIndexQueue.itemCount)
    {
        Index = popfront(&SchedulerThreadPoolIndexQueue);

        /* Find the target thread in queue */
        if (ThreadPool.TCB[Index].ThreadID == thread)
        {
            found = 1;
            TargetThreadTCBIndex = i;
            /* If the thread has already been reaped, then return ESRCH */
            if (ThreadPool.TCB[i].Status == REAPED)
            {
                return ESRCH;
            }
        }
        pushback(&SchedulerThreadPoolIndexQueue, Index);
        i++;
    }

    /* If not found, then return the errorno */
    if (!found)
    {
        return ESRCH;
    }

    /* If the thread has already exited, then just reap it */
    if (ThreadPool.TCB[TargetThreadTCBIndex].Status == EXITED)
    {
        ThreadPool.TCB[TargetThreadTCBIndex].ThreadID = 0;
        ThreadPool.TCB[TargetThreadTCBIndex].Status = REAPED;
        ThreadPool.TCB[TargetThreadTCBIndex].CallingThreadTCBIndex = -1;
        if (value_ptr != NULL)
        {
            *value_ptr = ThreadPool.TCB[TargetThreadTCBIndex].ReturnValue;
        }
        ThreadPool.TCB[TargetThreadTCBIndex].ReturnValue = NULL;
        return 0;
    }

    /* Block and store the TCB Index of calling thread */
    ThreadPool.TCB[TargetThreadTCBIndex].CallingThreadTCBIndex = CallingThreadTCBIndex;
    ThreadPool.TCB[CallingThreadTCBIndex].Status = BLOCKED;

    unlock();

    Scheduler();

    lock();

    if (value_ptr != NULL)
    {
        *value_ptr = ThreadPool.TCB[TargetThreadTCBIndex].ReturnValue;
    }

    /* Reap the Target Thread */
    ThreadPool.TCB[TargetThreadTCBIndex].ThreadID = 0;
    ThreadPool.TCB[TargetThreadTCBIndex].Status = REAPED;
    ThreadPool.TCB[TargetThreadTCBIndex].CallingThreadTCBIndex = -1;
    ThreadPool.TCB[TargetThreadTCBIndex].ReturnValue = NULL;

    unlock();

    /* Join succeeds */
    return 0;
}

void Scheduler()
{
    lock();
    /* If only one main thread, just return */
    if (size(&SchedulerThreadPoolIndexQueue) <= 1)
    {
        unlock();
        return;
    }

    int Index = peekfront(&SchedulerThreadPoolIndexQueue);

    if (setjmp(ThreadPool.TCB[Index].Registers) == 0)
    {
        /* Pushback the poped front Thread Pool Index of the saved thread to the end of queue */
        pushback(&SchedulerThreadPoolIndexQueue, Index);
        popfront(&SchedulerThreadPoolIndexQueue);
        Index = peekfront(&SchedulerThreadPoolIndexQueue);
        /* Loop through the queue if the front thread is blocked or exited */
        while (ThreadPool.TCB[Index].Status == BLOCKED || ThreadPool.TCB[Index].Status == EXITED || ThreadPool.TCB[Index].Status == REAPED)
        {
            /* If the thread is not reaped, then pushback the front Thread Pool Index of the saved thread to the end of queue */
            if (ThreadPool.TCB[Index].Status != REAPED)
            {
                pushback(&SchedulerThreadPoolIndexQueue, Index);
            }
            popfront(&SchedulerThreadPoolIndexQueue);
            Index = peekfront(&SchedulerThreadPoolIndexQueue);
        }
        if (ThreadPool.TCB[Index].JustCreated)
        {
            unlock();
            ThreadPool.TCB[Index].JustCreated = 0;
        }

        /* Longjmp to the front(next) thread registers */
        longjmp(ThreadPool.TCB[Index].Registers, 1);
    }
    else
    {
        unlock();
        return;
    }
}

void lock()
{
    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
    {
        perror("Error:");
        exit(1);
    }
}

void unlock()
{
    if (sigprocmask(SIG_UNBLOCK, &mask, NULL) < 0)
    {
        perror("Error:");
        exit(1);
    }
}

int sem_init(sem_t *sem, int pshared, unsigned int value)
{
    if (sem == NULL)
    {
        return -1;
    }
    if (pshared != 0)
    {
        return -1;
    }
    if (value > SEM_VALUE_MAX)
    {
        return -1;
    }
    int i;
    for (i = 0; i < MAX_SEMAPHORE; i++)
    {
        if (!SemPool.semaphore[i].Initialized)
        {
            SemaphoreID = i;
            break;
        }
    }

    lock();
    SemPool.semaphore[SemaphoreID].ID = SemaphoreID;
    SemPool.semaphore[SemaphoreID].Value = value;
    SemPool.semaphore[SemaphoreID].Initialized = 1;
    Queue[SemaphoreID].front = 0;
    Queue[SemaphoreID].rear = -1;
    Queue[SemaphoreID].itemCount = 0;
    sem->__align = SemPool.semaphore[SemaphoreID].ID;
    SemaphoreCount++;
    unlock();

    return 0;
}

int sem_destroy(sem_t *sem)
{
    if (sem == NULL)
    {
        return -1;
    }

    lock();

    int ID = sem->__align;

    if (!SemPool.semaphore[ID].Initialized)
    {
        return -1;
    }

    if (Queue[ID].itemCount > 0)
    {
        return -1;
    }

    SemPool.semaphore[ID].ID = 0;
    SemPool.semaphore[ID].Value = 0;
    SemPool.semaphore[ID].Initialized = 0;
    Queue[SemaphoreID].front = 0;
    Queue[SemaphoreID].rear = -1;
    Queue[SemaphoreID].itemCount = 0;
    SemaphoreCount--;

    unlock();

    return 0;
}

int sem_wait(sem_t *sem)
{
    if (sem == NULL)
    {
        return -1;
    }

    lock();

    int ID = sem->__align;

    if (!SemPool.semaphore[ID].Initialized)
    {
        return -1;
    }

    if (SemPool.semaphore[ID].Value > 0)
    {
        SemPool.semaphore[ID].Value -= 1;
        unlock();
    }
    else
    {
        int Index = peekfront(&SchedulerThreadPoolIndexQueue);
        ThreadPool.TCB[Index].Status = BLOCKED;
        pushback(&Queue[ID], Index);
        unlock();
        Scheduler();
    }
}

int sem_post(sem_t *sem)
{
    if (sem == NULL)
    {
        return -1;
    }

    lock();

    int ID = sem->__align;

    if (!SemPool.semaphore[ID].Initialized)
    {
        return -1;
    }

    if (SemPool.semaphore[ID].Value > 0)
    {
        SemPool.semaphore[ID].Value += 1;
        unlock();
    }
    else
    {
        int Index = popfront(&Queue[ID]);
        ThreadPool.TCB[Index].Status = ACTIVE;
        unlock();
        Scheduler();
    }
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
        Queue->IndexQueue[i] = Queue->IndexQueue[i - 1];
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
