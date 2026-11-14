#include "database_engine.h"

typedef struct ThreadPoolTask {
    ThreadPoolTaskFn fn;
    void* arg;
} ThreadPoolTask;

struct ThreadPool {
    int worker_count;
    int queue_capacity;
    ThreadPoolTask* queue;
    int queue_head;
    int queue_tail;
    int queue_count;
    atomic_bool shutdown;
#ifdef _WIN32
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE not_empty;
    CONDITION_VARIABLE not_full;
    HANDLE* threads;
#else
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    pthread_t* threads;
#endif
};

#ifdef _WIN32
static DWORD WINAPI thread_pool_worker_fn(LPVOID arg)
#else
static void* thread_pool_worker_fn(void* arg)
#endif
{
    ThreadPool* pool = (ThreadPool*)arg;
    while (1) {
        ThreadPoolTask task;
#ifdef _WIN32
        EnterCriticalSection(&pool->lock);
        while (pool->queue_count == 0 && !atomic_load(&pool->shutdown)) {
            SleepConditionVariableCS(&pool->not_empty, &pool->lock, INFINITE);
        }
        if (atomic_load(&pool->shutdown) && pool->queue_count == 0) {
            LeaveCriticalSection(&pool->lock);
            break;
        }
        task = pool->queue[pool->queue_head];
        pool->queue_head = (pool->queue_head + 1) % pool->queue_capacity;
        pool->queue_count--;
        WakeConditionVariable(&pool->not_full);
        LeaveCriticalSection(&pool->lock);
#else
        pthread_mutex_lock(&pool->lock);
        while (pool->queue_count == 0 && !atomic_load(&pool->shutdown)) {
            pthread_cond_wait(&pool->not_empty, &pool->lock);
        }
        if (atomic_load(&pool->shutdown) && pool->queue_count == 0) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }
        task = pool->queue[pool->queue_head];
        pool->queue_head = (pool->queue_head + 1) % pool->queue_capacity;
        pool->queue_count--;
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->lock);
#endif
        if (task.fn) {
            task.fn(task.arg);
        }
    }
    return 0;
}

ThreadPool* thread_pool_create(int num_workers, int queue_capacity) {
    if (num_workers <= 0) num_workers = 4;
    if (queue_capacity <= 0) queue_capacity = 256;

    ThreadPool* pool = (ThreadPool*)calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->worker_count = num_workers;
    pool->queue_capacity = queue_capacity;
    pool->queue = (ThreadPoolTask*)calloc((size_t)queue_capacity, sizeof(ThreadPoolTask));
    if (!pool->queue) {
        free(pool);
        return NULL;
    }
    atomic_init(&pool->shutdown, false);

#ifdef _WIN32
    InitializeCriticalSection(&pool->lock);
    InitializeConditionVariable(&pool->not_empty);
    InitializeConditionVariable(&pool->not_full);
    pool->threads = (HANDLE*)calloc((size_t)num_workers, sizeof(HANDLE));
    if (!pool->threads) {
        DeleteCriticalSection(&pool->lock);
        free(pool->queue);
        free(pool);
        return NULL;
    }
    for (int i = 0; i < num_workers; i++) {
        pool->threads[i] = CreateThread(NULL, 0, thread_pool_worker_fn, pool, 0, NULL);
    }
#else
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->not_empty, NULL);
    pthread_cond_init(&pool->not_full, NULL);
    pool->threads = (pthread_t*)calloc((size_t)num_workers, sizeof(pthread_t));
    if (!pool->threads) {
        pthread_mutex_destroy(&pool->lock);
        pthread_cond_destroy(&pool->not_empty);
        pthread_cond_destroy(&pool->not_full);
        free(pool->queue);
        free(pool);
        return NULL;
    }
    for (int i = 0; i < num_workers; i++) {
        pthread_create(&pool->threads[i], NULL, thread_pool_worker_fn, pool);
    }
#endif

    return pool;
}

bool thread_pool_submit(ThreadPool* pool, ThreadPoolTaskFn fn, void* arg) {
    if (!pool || !fn || atomic_load(&pool->shutdown)) return false;

#ifdef _WIN32
    EnterCriticalSection(&pool->lock);
    if (pool->queue_count >= pool->queue_capacity) {
        LeaveCriticalSection(&pool->lock);
        return false;
    }
    pool->queue[pool->queue_tail].fn = fn;
    pool->queue[pool->queue_tail].arg = arg;
    pool->queue_tail = (pool->queue_tail + 1) % pool->queue_capacity;
    pool->queue_count++;
    WakeConditionVariable(&pool->not_empty);
    LeaveCriticalSection(&pool->lock);
#else
    pthread_mutex_lock(&pool->lock);
    if (pool->queue_count >= pool->queue_capacity) {
        pthread_mutex_unlock(&pool->lock);
        return false;
    }
    pool->queue[pool->queue_tail].fn = fn;
    pool->queue[pool->queue_tail].arg = arg;
    pool->queue_tail = (pool->queue_tail + 1) % pool->queue_capacity;
    pool->queue_count++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
#endif

    return true;
}

void thread_pool_destroy(ThreadPool* pool) {
    if (!pool) return;
    atomic_store(&pool->shutdown, true);

#ifdef _WIN32
    EnterCriticalSection(&pool->lock);
    WakeAllConditionVariable(&pool->not_empty);
    LeaveCriticalSection(&pool->lock);

    for (int i = 0; i < pool->worker_count; i++) {
        if (pool->threads[i]) {
            WaitForSingleObject(pool->threads[i], 3000);
            CloseHandle(pool->threads[i]);
        }
    }
    DeleteCriticalSection(&pool->lock);
    free(pool->threads);
#else
    pthread_mutex_lock(&pool->lock);
    pthread_cond_broadcast(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);

    for (int i = 0; i < pool->worker_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);
    free(pool->threads);
#endif

    free(pool->queue);
    free(pool);
}

int thread_pool_worker_count(const ThreadPool* pool) {
    return pool ? pool->worker_count : 0;
}

int thread_pool_queue_depth(const ThreadPool* pool) {
    if (!pool) return 0;
    int count = 0;
#ifdef _WIN32
    EnterCriticalSection((CRITICAL_SECTION*)&pool->lock);
    count = pool->queue_count;
    LeaveCriticalSection((CRITICAL_SECTION*)&pool->lock);
#else
    pthread_mutex_lock((pthread_mutex_t*)&pool->lock);
    count = pool->queue_count;
    pthread_mutex_unlock((pthread_mutex_t*)&pool->lock);
#endif
    return count;
}
