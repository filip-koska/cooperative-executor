#include "future_combinators.h"
#include <stdlib.h>

#include "future.h"
#include "waker.h"
#include "executor.h"
#include "err.h"

#define FIRST_SUBTASK 1
#define SECOND_SUBTASK 2

static FutureState future_then_progress(Future *base, Mio *mio, Waker waker) {
    ThenFuture *self = (ThenFuture*)base;
    FutureState ret_val;
    if (!self->fut1_completed) {
        ret_val = (*self->fut1->progress)(self->fut1, mio, waker);
        if (ret_val == FUTURE_PENDING) // fut1 waiting on descriptor; ensured that ThenFuture will be woken
            return FUTURE_PENDING;
        if (ret_val == FUTURE_FAILURE) {
            self->base.errcode = THEN_FUTURE_ERR_FUT1_FAILED;
            return FUTURE_FAILURE;
        }
        self->fut1_completed = true;
        self->fut2->arg = self->fut1->ok;
    }
    if (self->fut1_completed) {
        ret_val = (*self->fut2->progress)(self->fut2, mio, waker);
        if (ret_val == FUTURE_PENDING)
            return FUTURE_PENDING;
        if (ret_val == FUTURE_FAILURE) {
            self->base.errcode = THEN_FUTURE_ERR_FUT2_FAILED;
            return FUTURE_FAILURE;
        }
        // fut2 returned COMPLETED
        self->base.ok = self->fut2->ok;
        return FUTURE_COMPLETED;
    }
    return FUTURE_PENDING;
}

ThenFuture future_then(Future* fut1, Future* fut2)
{
    return (ThenFuture) {
        .base = future_create(future_then_progress),
        .fut1 = fut1,
        .fut2 = fut2,
        .fut1_completed = false,
    };
}

typedef struct JoinSubFuture {
    Future base;
    JoinFuture *parent;
    Future *subtask;
    int which;
    Waker parent_waker;
} JoinSubFuture;

static FutureState future_join_sub_progress(Future *base, Mio *mio, Waker waker) {
    JoinSubFuture *self = (JoinSubFuture*)base;
    FutureState ret = (*self->subtask->progress)(self->subtask, mio, waker);
    if (ret == FUTURE_PENDING)
        return FUTURE_PENDING;
    if (self->which == FIRST_SUBTASK) { // code for subtask 1
        self->parent->result.fut1.errcode = self->subtask->errcode;
        self->parent->result.fut1.ok = self->subtask->ok;
        if (ret == FUTURE_FAILURE) {
            if (self->parent->result.fut2.errcode != FUTURE_SUCCESS) { // the other subtask had also failed
                self->parent->base.errcode = JOIN_FUTURE_ERR_BOTH_FUTS_FAILED;
                waker_wake(&self->parent_waker);
            } else {
                self->parent->base.errcode = JOIN_FUTURE_ERR_FUT1_FAILED;
                if (self->parent->fut2_completed)
                    waker_wake(&self->parent_waker);
            }
        } else { // subtask 1 returned COMPLETED
            self->parent->fut1_completed = true;
            if (self->parent->fut2_completed || self->parent->base.errcode == JOIN_FUTURE_ERR_FUT2_FAILED)
                waker_wake(&self->parent_waker);
        }
    } else { // code for subtask 2
        self->parent->result.fut2.errcode = self->subtask->errcode;
        self->parent->result.fut2.ok = self->subtask->ok;
        if (ret == FUTURE_FAILURE) {
            if (self->parent->result.fut1.errcode != FUTURE_SUCCESS) {
                self->parent->base.errcode = JOIN_FUTURE_ERR_BOTH_FUTS_FAILED;
                waker_wake(&self->parent_waker);
            } else {
                self->parent->base.errcode = JOIN_FUTURE_ERR_FUT2_FAILED;
                if (self->parent->fut1_completed)
                    waker_wake(&self->parent_waker);
            }
        } else { // subtask 2 returned COMPLETED
            self->parent->fut2_completed = true;
            if (self->parent->fut1_completed || self->parent->base.errcode == JOIN_FUTURE_ERR_FUT1_FAILED)
                waker_wake(&self->parent_waker);
        }
    }
    // free(self);
    return ret;
}

JoinSubFuture* future_sub_join(JoinFuture *parent, Future *subtask, int which, Waker parent_waker) {
    JoinSubFuture *ret = (JoinSubFuture*)malloc(sizeof(JoinSubFuture));
    if (!ret)
        return NULL;
    ret->base = future_create(future_join_sub_progress);
    ret->parent = parent;
    ret->subtask = subtask;
    ret->which = which;
    ret->parent_waker = parent_waker;
    return ret;
}

static FutureState future_join_progress(Future *base, Mio *mio, Waker waker) {
    JoinFuture *self = (JoinFuture*)base;
    // this is the first invokation of this function
    // iff both subtasks are not completed and both errcodes indicate SUCCESS
    if (!self->fut1_completed && !self->fut2_completed && self->result.fut1.errcode == FUTURE_SUCCESS
            && self->result.fut2.errcode == FUTURE_SUCCESS) {
        JoinSubFuture *sub1 = future_sub_join(self, self->fut1, FIRST_SUBTASK, waker);
        JoinSubFuture *sub2 = future_sub_join(self, self->fut2, SECOND_SUBTASK, waker);
        if (!sub1 || !sub2)
            fatal("Allocation failed\n");
        self->fut1 = (Future*)sub1;
        self->fut2 = (Future*)sub2;
        executor_spawn((Executor*)waker.executor, self->fut1);
        executor_spawn((Executor*)waker.executor, self->fut2);
        return FUTURE_PENDING;
    }
    FutureState ret;
    if (self->fut1_completed && self->fut2_completed)
        ret = FUTURE_COMPLETED;
    else
        ret = FUTURE_FAILURE;
    JoinSubFuture *jsf1 = (JoinSubFuture*)self->fut1;
    JoinSubFuture *jsf2 = (JoinSubFuture*)self->fut2;
    self->fut1 = jsf1->subtask;
    self->fut2 = jsf2->subtask;
    free(jsf1);
    free(jsf2);
    return ret;
}

JoinFuture future_join(Future *fut1, Future *fut2) {
    return (JoinFuture) {
        .base = future_create(future_join_progress),
        .fut1 = fut1,
        .fut1_completed = false,
        .fut2 = fut2,
        .fut2_completed = false,
        .result.fut1.errcode = FUTURE_SUCCESS,
        .result.fut2.errcode = FUTURE_SUCCESS,
    };
}

typedef struct SelectSubFuture {
    Future base;
    Future *subtask;
    struct SelectSubFuture *other;
    SelectFuture *parent;
    int which;
    bool unneeded;
    Waker parent_waker;
} SelectSubFuture;

static FutureState future_select_sub_progress(Future *base, Mio *mio, Waker waker) {
    SelectSubFuture *self = (SelectSubFuture*)base;
    if (mio == NULL) {
        free(self);
        return FUTURE_FAILURE;
    }
    if (self->unneeded)
        return FUTURE_PENDING;
    FutureState ret = (*self->subtask->progress)(self->subtask, mio, waker);
    if (ret == FUTURE_PENDING)
        return FUTURE_PENDING;
    if (self->which == FIRST_SUBTASK) {
        if (ret == FUTURE_FAILURE) {
            if (self->parent->which_completed == SELECT_COMPLETED_NONE)
                self->parent->which_completed = SELECT_FAILED_FUT1;
            else { // self->parent->which_completed == SELECT_FAILED_FUT2
                self->parent->which_completed = SELECT_FAILED_BOTH;
                self->parent->base.errcode = self->subtask->errcode;
                waker_wake(&self->parent_waker);
            }
        } else { // task 1 returned COMPLETED first
            self->parent->base.ok = self->subtask->ok;
            self->parent->which_completed = SELECT_COMPLETED_FUT1;
            self->other->unneeded = true;
            waker_wake(&self->parent_waker);
        }
    } else {
        if (ret == FUTURE_FAILURE) {
            if (self->parent->which_completed == SELECT_COMPLETED_NONE)
                self->parent->which_completed = SELECT_FAILED_FUT2;
            else {
                self->parent->which_completed = SELECT_FAILED_BOTH;
                self->parent->base.errcode = self->subtask->errcode;
                waker_wake(&self->parent_waker);
            }
        } else {
            self->parent->base.ok = self->subtask->ok;
            self->parent->which_completed = SELECT_COMPLETED_FUT2;
            self->other->unneeded = true;
            waker_wake(&self->parent_waker);
        }
    }
    return FUTURE_PENDING;
}

SelectSubFuture* future_select_sub(Future *subtask, SelectFuture *parent, int which, Waker parent_waker) {
    SelectSubFuture *ret = (SelectSubFuture*)malloc(sizeof(SelectSubFuture));
    if (!ret)
        return NULL;
    ret->base = future_create(future_select_sub_progress);
    ret->subtask = subtask;
    ret->which = which;
    ret->unneeded = false;
    ret->parent_waker = parent_waker;
    return ret;
}

static FutureState future_select_progress(Future *base, Mio *mio, Waker waker) {
    SelectFuture *self = (SelectFuture*)base;
    if (self->which_completed == SELECT_COMPLETED_NONE) {
        SelectSubFuture *sub1 = future_select_sub(self->fut1, self, FIRST_SUBTASK, waker);
        SelectSubFuture *sub2 = future_select_sub(self->fut2, self, SECOND_SUBTASK, waker);
        if (!sub1 || !sub2)
            fatal("Allocation failed\n");
        sub1->other = sub2;
        sub2->other = sub1;
        self->fut1 = (Future*)sub1;
        self->fut2 = (Future*)sub2;
        Waker tmp_waker;
        tmp_waker.executor = waker.executor;
        tmp_waker.future = self->fut1;
        waker_wake(&tmp_waker);
        tmp_waker.future = self->fut2;
        waker_wake(&tmp_waker);
        return FUTURE_PENDING;
    }
    SelectSubFuture *ssf1 = (SelectSubFuture*)self->fut1;
    SelectSubFuture *ssf2 = (SelectSubFuture*)self->fut2;
    if (self->which_completed == SELECT_FAILED_BOTH) {
        free(ssf1);
        free(ssf2);
        return FUTURE_FAILURE;
    }
    if (self->which_completed == SELECT_COMPLETED_FUT1) {
        free(ssf1);
        return FUTURE_COMPLETED;
    }
    if (self->which_completed == SELECT_COMPLETED_FUT2) {
        free(ssf2);
        return FUTURE_COMPLETED;
    }
    return FUTURE_PENDING;
}

SelectFuture future_select(Future *fut1, Future *fut2) {
    return (SelectFuture) {
        .base = future_create(future_select_progress),
        .fut1 = fut1,
        .fut2 = fut2,
        .which_completed = SELECT_COMPLETED_NONE,
    };
}