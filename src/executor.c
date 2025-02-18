#include "executor.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#include "debug.h"
#include "future.h"
#include "mio.h"
#include "waker.h"
#include "err.h"

// Structure to represent the current-thread executor.


typedef struct Queue Queue;

/**
 * Cyclic buffer task queue
 * Semantics:
 * The queue is valid and empty iff size == 0;
 * If size == 0, head and tail point to the same unspecified free index;
 * If size > 0, head is the index of the first available position,
 * and tail is the index of the last element.
 */
struct Queue {
    Future **data; // internal array
    size_t max_size;
    size_t head;
    size_t tail;
    size_t size;
};

void queue_init(Queue *queue, size_t max_queue_size) {
    if (!queue)
        return;
    queue->data = (Future**)malloc(max_queue_size * sizeof(Future*));
    if (!queue->data)
        fatal("Allocation failed\n");
    queue->max_size = max_queue_size;
    queue->head = queue->tail = queue->size = 0;
}

bool queue_empty(Queue *queue) {
    return queue->size == 0;
}

void queue_enqueue_future(Queue *queue, Future *future) {
    if (!future)
        fatal("NULL Future pointer\n");
    if (queue->size == queue->max_size)
        fatal("Assignment guarantees violated: size of queue exceeds max_queue_size\n");
    queue->data[queue->head] = future;
    queue->head = (queue->head + 1) % queue->max_size;
    ++queue->size;
}

Future *queue_dequeue_future(Queue *queue) {
    if (queue_empty(queue))
        return NULL;
    Future *ret = queue->data[queue->tail];
    queue->tail = (queue->tail + 1) % queue->max_size;
    --queue->size;
    return ret;
}

void queue_destroy(Queue *queue) {
    free(queue->data);
}


struct Executor {
    Queue queue;
    Mio *mio;
    size_t needed_tasks;
};


Executor* executor_create(size_t max_queue_size) {
    Executor *executor = (Executor*)malloc(sizeof(Executor));
    if (!executor)
        fatal("Allocation failed\n");
    queue_init(&executor->queue, max_queue_size);
    executor->mio = mio_create(executor);
    if (!executor->mio)
        fatal("Mio construction failed\n");
    executor->needed_tasks = 0;
    return executor;
}

// Wake a task that had already been spawned
void waker_wake(Waker* waker) {
    Executor *tmp = (Executor*)(waker->executor);
    queue_enqueue_future(&tmp->queue, waker->future);
}

// Spawn a new independent task and update needeed task counter
void executor_spawn(Executor* executor, Future* fut) {
    fut->is_active = true;
    queue_enqueue_future(&executor->queue, fut);
    ++executor->needed_tasks;
}

void executor_run(Executor* executor) {
    int finished = 0;
    // Try to progress tasks until all spawned tasks have been finished
    while (finished < executor->needed_tasks) {
        if (!queue_empty(&executor->queue)) {
            Future *fut = queue_dequeue_future(&executor->queue);
            Waker waker;
            waker.executor = (void*)executor;
            waker.future = fut;
            FutureState fs = (*fut->progress)(fut, executor->mio, waker);
            if (fs != FUTURE_PENDING) { // future finished computation
                ++finished;
                fut->is_active = false;
            }
        } else { // No active tasks but some are still pending
            mio_poll(executor->mio);
        }
    }
}

void executor_destroy(Executor* executor) {
    // All Futures remaining are unneded subtasks of SelectFutures;
    // Only now can we free their wrappers
    while (!queue_empty(&executor->queue)) {
        Future *fut = queue_dequeue_future(&executor->queue);
        Waker waker;
        (*fut->progress)(fut, NULL, waker);
    }
    queue_destroy(&executor->queue);
    mio_destroy(executor->mio);
    free(executor);
}
