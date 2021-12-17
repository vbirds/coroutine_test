/*
 * Tencent is pleased to support the open source community by making Pebble available.
 * Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.
 * Licensed under the MIT License (the "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied. See the License for the specific language governing permissions and limitations under
 * the License.
 *
 */


#include <assert.h>
#include <cstdio>
#include <cstdlib>
#include <errno.h>
#include <iostream>
#include <set>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "common/coroutine.h"
#include "common/log.h"
#include "common/timer.h"

namespace pebble {


struct coroutine *
_co_new(struct schedule *S, cxx::function<void()>& std_func) {
    if (NULL == S) {
        assert(0);
        return NULL;
    }

    struct coroutine * co = NULL;
    if (S->co_free_list.empty()) {
        co = new coroutine;
        co->stack = new char[S->stack_size];
    } else {
        co = S->co_free_list.front();
        S->co_free_list.pop_front();

        S->co_free_num--;
    }

    co->std_func = std_func;
    co->func = NULL;
    co->ud = NULL;
    co->sch = S;
    co->status = COROUTINE_READY;

    return co;
}

struct coroutine *
_co_new(struct schedule *S, coroutine_func func, void *ud) {
    if (NULL == S) {
        assert(0);
        return NULL;
    }

    struct coroutine * co = NULL;

    if (S->co_free_list.empty()) {
        co = new coroutine;
        co->stack = new char[S->stack_size];
    } else {
        co = S->co_free_list.front();
        S->co_free_list.pop_front();

        S->co_free_num--;
    }
    co->func = func;
    co->ud = ud;
    co->sch = S;
    co->status = COROUTINE_READY;

    return co;
}

void _co_delete(struct coroutine *co) {
    delete [] co->stack;
    delete co;
}

struct schedule *
coroutine_open(uint32_t stack_size) {
    if (0 == stack_size) {
        stack_size = 256 * 1024;
    }

    struct schedule *S = new schedule;
    S->nco = 0;
    S->running = -1;
    S->co_free_num = 0;
    S->stack_size = stack_size;

    PLOG_INFO("coroutine_open is called.");
    return S;
}

void coroutine_close(struct schedule *S) {

    if (NULL == S) {
        return;
    }

    // 遍历所有的协程，逐个释放
    cxx::unordered_map<int64_t, coroutine*>::iterator pos = S->co_hash_map.begin();
    for (; pos != S->co_hash_map.end(); pos++) {
        if (pos->second) {
            _co_delete(pos->second);
        }
    }

    std::list<coroutine*>::iterator p = S->co_free_list.begin();
    for (; p != S->co_free_list.end(); p++) {
        _co_delete(*p);
    }

    // 释放掉整个调度器
    delete S;
    S = NULL;
}
int64_t coroutine_new(struct schedule *S, cxx::function<void()>& std_func) {
    if (NULL == S) {
        return -1;
    }
    struct coroutine *co = _co_new(S, std_func);
    int64_t id = S->nco;
    S->co_hash_map[id] = co;
    S->nco++;

    PLOG_TRACE("coroutine %ld is created.", id);
    return id;
}

int64_t coroutine_new(struct schedule *S, coroutine_func func, void *ud) {
    if (NULL == S || NULL == func) {
        return -1;
    }
    struct coroutine *co = _co_new(S, func, ud);
    int64_t id = S->nco;
    S->co_hash_map[id] = co;
    S->nco++;

    PLOG_TRACE("coroutine %ld is created.", id);
    return id;
}

static void mainfunc(uint32_t low32, uint32_t hi32) {
    uintptr_t ptr = (uintptr_t) low32 | ((uintptr_t) hi32 << 32);
    struct schedule *S = (struct schedule *) ptr;
    int64_t id = S->running;
    struct coroutine *C = S->co_hash_map[id];
    if (C->func != NULL) {
        C->func(S, C->ud);
    } else {
        C->std_func();
    }
    S->co_free_list.push_back(C);
    S->co_free_num++;

    if (S->co_free_num > MAX_FREE_CO_NUM) {
        coroutine* co = S->co_free_list.front();
        _co_delete(co);

        S->co_free_list.pop_front();
        S->co_free_num--;
    }

    S->co_hash_map.erase(id);
    S->running = -1;
    PLOG_TRACE("coroutine %ld is deleted.", id);
}

int32_t coroutine_resume(struct schedule * S, int64_t id, int32_t result) {
    if (NULL == S) {
        return kCO_INVALID_PARAM;
    }
    if (S->running != -1) {
        return kCO_CANNOT_RESUME_IN_COROUTINE;
    }
    cxx::unordered_map<int64_t, coroutine*>::iterator pos = S->co_hash_map.find(id);

    // 如果在协程哈希表中没有找到，或者找到的协程内容为空
    if (pos == S->co_hash_map.end()) {
        PLOG_ERROR("coroutine %ld can't find in co_map", id);
        return kCO_COROUTINE_UNEXIST;
    }

    struct coroutine *C = pos->second;
    if (NULL == C) {
        PLOG_ERROR("coroutine %ld instance is NULL", id);
        return kCO_COROUTINE_UNEXIST;
    }

    C->result = result;
    int status = C->status;
    switch (status) {
        case COROUTINE_READY: {
            PLOG_TRACE("coroutine %ld status is COROUTINE_READY, begin to execute...", id);

            getcontext(&C->ctx);
            C->ctx.uc_stack.ss_sp = C->stack;
            C->ctx.uc_stack.ss_size = S->stack_size;
            C->ctx.uc_stack.ss_flags = 0;
            C->ctx.uc_link = &S->main;
            S->running = id;
            C->status = COROUTINE_RUNNING;
            uintptr_t ptr = (uintptr_t) S;
            makecontext(&C->ctx, (void (*)(void)) mainfunc, 2,
            (uint32_t)ptr,  // NOLINT
            (uint32_t)(ptr>>32));  // NOLINT

            swapcontext(&S->main, &C->ctx);

            break;
        }
        case COROUTINE_SUSPEND: {
            PLOG_TRACE("coroutine %ld status is COROUTINE_SUSPEND,"
                    "begin to resume...", id);

            S->running = id;
            C->status = COROUTINE_RUNNING;
            swapcontext(&S->main, &C->ctx);

            break;
        }

        default:
            PLOG_DEBUG("coroutine %ld status is failed, can not to be resume...", id);
            return kCO_COROUTINE_STATUS_ERROR;
    }

    return 0;
}

int32_t coroutine_yield(struct schedule * S) {
    if (NULL == S) {
        return kCO_INVALID_PARAM;
    }

    int64_t id = S->running;
    if (id < 0) {
        PLOG_ERROR("have no running coroutine, can't yield.");
        return kCO_NOT_IN_COROUTINE;
    }

    assert(id >= 0);
    struct coroutine * C = S->co_hash_map[id];

    if (C->status != COROUTINE_RUNNING) {
        PLOG_ERROR("coroutine %ld status is SUSPEND, can't yield again.", id);
        return kCO_NOT_RUNNING;
    }

    C->status = COROUTINE_SUSPEND;
    S->running = -1;

    PLOG_TRACE("coroutine %ld will be yield, swith to main loop...", id);
    swapcontext(&C->ctx, &S->main);

    return C->result;
}

int coroutine_status(struct schedule * S, int64_t id) {
    // assert(id >= 0 && id <= S->nco);
    if (NULL == S) {
        return COROUTINE_DEAD;
    }

    if (id < 0 || id > S->nco) {
        PLOG_DEBUG("coroutine %ld not exist", id);
        return COROUTINE_DEAD;
    }

    cxx::unordered_map<int64_t, coroutine*>::iterator pos = S->co_hash_map.find(id);

    // 如果在协程哈希表中没有找到，或者找到的协程内容为空
    if (pos == S->co_hash_map.end()) {
        PLOG_DEBUG("cann't find coroutine %ld", id);
        return COROUTINE_DEAD;
    }

    return (pos->second)->status;
}

int64_t coroutine_running(struct schedule * S) {
    if (NULL == S) {
        return -1;
    }

    return S->running;
}


void DoTask(struct schedule*, void *ud) {
    CoroutineTask* task = static_cast<CoroutineTask*>(ud);
    assert(task != NULL);
    task->Run();
    delete task;
}

CoroutineTask::CoroutineTask()
        : id_(-1),
          schedule_obj_(NULL) {
    // DO NOTHING
}

CoroutineTask::~CoroutineTask() {
    if (schedule_obj_ == NULL)
        return;

    // 如果schedule_obj_没进入Close()流程
    if (schedule_obj_->schedule_ != NULL) {
        if (id_ == -1) {
            schedule_obj_->pre_start_task_.erase(this);
        } else {
            // 防止schedule_在清理时重复delete自己
            schedule_obj_->task_map_.erase(id_);
        }
    }
}

int64_t CoroutineTask::Start(bool is_immediately) {
    if (is_immediately && schedule_obj_->CurrentTaskId() != INVALID_CO_ID) {
        delete this;
        return -1;
    }
    id_ = coroutine_new(schedule_obj_->schedule_, DoTask, this);
    if (id_ < 0)
        id_ = -1;
    int64_t id = id_;
    schedule_obj_->task_map_[id_] = this;
    schedule_obj_->pre_start_task_.erase(this);
    if (is_immediately) {
        int32_t ret = coroutine_resume(schedule_obj_->schedule_, id_);
        if (ret != 0) {
            id = -1;
        }
    }
    return id;
}

CoroutineSchedule::CoroutineSchedule()
        : schedule_(NULL),
          timer_(NULL),
          task_map_(),
          pre_start_task_() {
    // DO NOTHING
}

CoroutineSchedule::~CoroutineSchedule() {
    if (schedule_ != NULL)
        Close();
}

int CoroutineSchedule::Init(Timer* timer, uint32_t stack_size) {
    timer_ = timer;
    schedule_ = coroutine_open(stack_size);
    if (schedule_ == NULL)
        return -1;
    return 0;
}

int CoroutineSchedule::Close() {
    int ret = 0;
    if (schedule_ != NULL) {
        coroutine_close(schedule_);
        schedule_ = NULL;
    }

    timer_ = NULL;

    ret += pre_start_task_.size();
    std::set<CoroutineTask*>::iterator pre_it;
    for (pre_it = pre_start_task_.begin(); pre_it != pre_start_task_.end();
            ++pre_it) {
        delete *pre_it;
    }

    ret += task_map_.size();
    cxx::unordered_map<int64_t, CoroutineTask*>::iterator it;
    for (it = task_map_.begin(); it != task_map_.end();) {
        delete it->second;

        // 为了安全的删除迭代器指向的对象，所以先++后调用
        task_map_.erase(it++);
    }

    return ret;
}

int CoroutineSchedule::Size() const {
    int ret = 0;
    ret += task_map_.size();
    ret += pre_start_task_.size();
    return ret;
}

CoroutineTask* CoroutineSchedule::CurrentTask() const {
    int64_t id = coroutine_running(schedule_);
    return Find(id);
}

CoroutineTask* CoroutineSchedule::Find(int64_t id) const {
    CoroutineTask* ret = NULL;
    cxx::unordered_map<int64_t, CoroutineTask*>::const_iterator it = task_map_.find(id);
    if (it != task_map_.end())
        ret = it->second;
    return ret;
}

int64_t CoroutineSchedule::CurrentTaskId() const {
    return coroutine_running(schedule_);
}

int CoroutineSchedule::AddTaskToSchedule(CoroutineTask* task) {
    task->schedule_obj_ = this;
    pre_start_task_.insert(task);
    return 0;
}

int32_t CoroutineSchedule::Yield(int32_t timeout_ms) {
    int64_t timerid = -1;
    int64_t co_id   = INVALID_CO_ID;
    if (timer_ && timeout_ms > 0) {
        co_id = CurrentTaskId();
        if (INVALID_CO_ID == co_id) {
            return kCO_NOT_IN_COROUTINE;
        }
        timerid = timer_->StartTimer(timeout_ms,
            cxx::bind(&CoroutineSchedule::OnTimeout, this, co_id));
        if (timerid < 0) {
            return kCO_START_TIMER_FAILED;
        }
    }

    int32_t ret = coroutine_yield(this->schedule_);

    if (timerid >= 0) {
        timer_->StopTimer(timerid);
    }

    return ret;
}

int32_t CoroutineSchedule::Resume(int64_t id, int32_t result) {
    return coroutine_resume(this->schedule_, id, result);
}

int CoroutineSchedule::Status(int64_t id) {
    return coroutine_status(this->schedule_, id);
}

int32_t CoroutineSchedule::OnTimeout(int64_t id) {
    Resume(id, kCO_TIMEOUT);
    return kTIMER_BE_REMOVED;
}

int32_t CoroutineTask::Yield(int32_t timeout_ms) {
    return schedule_obj_->Yield(timeout_ms);
}

CoroutineSchedule* CoroutineTask::schedule_obj() {
    return schedule_obj_;
}

} // namespace pebble
