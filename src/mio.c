#include "mio.h"

#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>

#include "debug.h"
#include "executor.h"
#include "waker.h"
#include "err.h"

// Structure to handle OS communication and descriptor tracking

// Maximum number of events to handle per epoll_wait call.
#define MAX_EVENTS 64

// Maximum number of descriptors that can be registered in epoll instance
#define MAX_DESCRIPTORS 1048577

struct Mio {
    struct epoll_event dummy; // dummy epoll_event for portability
    // so that a non-NULL pointer can be passed to epoll_ctl with EPOLL_CTL_DEL option
    Executor *executor;
    int epfd; // descriptor of epoll instance
    struct epoll_event events[MAX_EVENTS]; // helper array for epoll_wait
    int n_descriptors; // number of registered fds
};

// Create a new Mio instance
Mio* mio_create(Executor* executor) {
    Mio *ret = (Mio*)malloc(sizeof(Mio));
    if (!ret)
        fatal("Allocation failed\n");
    ret->executor = executor;
    ret->epfd = epoll_create(MAX_DESCRIPTORS);
    ASSERT_SYS_OK(ret->epfd);
    ret->n_descriptors = 0;
    return ret;
}

// Destroy a Mio instance
void mio_destroy(Mio* mio) {
    close(mio->epfd);
    free(mio);
}

// Register a new fd in epoll instance, or modify the events associated with one
int mio_register(Mio* mio, int fd, uint32_t events, Waker waker)
{
    debug("Registering (in Mio = %p) fd = %d with\n", mio, fd);

    ++mio->n_descriptors;

    struct epoll_event ee;
    ee.events = events;
    ee.data.ptr = (void*)waker.future;
    int create_res = epoll_ctl(mio->epfd, EPOLL_CTL_ADD, fd, &ee);
    if (create_res == -1 && errno == EEXIST) { // attempt to change events associated with descriptor
        --mio->n_descriptors;
        create_res = epoll_ctl(mio->epfd, EPOLL_CTL_MOD, fd, &ee);
    }
    return create_res;
}

// Unregister fd from Mio instance
int mio_unregister(Mio* mio, int fd)
{
    debug("Unregistering (from Mio = %p) fd = %d\n", mio, fd);

    int ret = epoll_ctl(mio->epfd, EPOLL_CTL_DEL, fd, &mio->dummy);
    if (ret == 0)
        --mio->n_descriptors;
    return ret;
}

// Wait for available I/O operations on registered fds
void mio_poll(Mio* mio)
{
    debug("Mio (%p) polling\n", mio);

    if (mio->n_descriptors == 0)
        return;

    int n_ready = epoll_wait(mio->epfd, mio->events, MAX_EVENTS, -1);

    // for (int i = 0; i < n_ready; ++i) {
    //     waker_wake(&mio->wakers[mio->events[i].data.fd]);
    // }
    Waker waker;
    waker.executor = (void*)mio->executor;
    for (int i = 0; i < n_ready; ++i) {
        waker.future = (Future*)mio->events[i].data.ptr;
        waker_wake(&waker);
    }
}
