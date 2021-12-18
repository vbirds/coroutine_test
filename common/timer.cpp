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

#include <algorithm>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "common/time_utility.h"
#include "common/timer.h"

#ifndef offsetof
#define offsetof(s,m) (size_t)(&reinterpret_cast<const volatile char&>((((s *)0x1)->m)) - 0x1)
#endif

#ifndef container
#define container(TYPE, MEMBER, pMember) (NULL == pMember ? NULL:((TYPE*)((size_t)(pMember)-offsetof(TYPE,MEMBER))))
#endif


namespace pebble {


#if 0
FdTimer::FdTimer() {
    m_max_timer_num = 1024;
    m_timer_seqid   = 0;
    m_last_error[0] = 0;
    // 2.6.8以上内核版本参数无效，填128仅做内核版本兼容性保证
    m_epoll_fd = epoll_create(128);
    if (m_epoll_fd < 0) {
        _LOG_LAST_ERROR("epoll_create failed(%s)", strerror(errno));
    }
}

FdTimer::~FdTimer() {
    int32_t fd = -1;
    cxx::unordered_map<int64_t, TimerItem*>::iterator it = m_timers.begin();
    for (; it != m_timers.end(); ++it) {
        fd = it->second->fd;
        if (epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
            _LOG_LAST_ERROR("epoll_ctl del %d failed(%s)", fd, strerror(errno));
        }

        close(fd);

        delete it->second;
        it->second = NULL;
    }

    m_timers.clear();

    if (m_epoll_fd >= 0) {
        close(m_epoll_fd);
    }
}

int64_t FdTimer::StartTimer(uint32_t timeout_ms, const TimeoutCallback& cb) {
    if (!cb || 0 == timeout_ms) {
        _LOG_LAST_ERROR("param is invalid: timeout_ms = %u, cb = %d", timeout_ms, (cb ? true : false));
        return kTIMER_INVALID_PARAM;
    }
    if (m_timers.size() >= m_max_timer_num) {
        _LOG_LAST_ERROR("timer over the limit(%d)", m_max_timer_num);
        return kTIMER_NUM_OUT_OF_RANGE;
    }

    // 创建timer fd
    int32_t fd = timerfd_create(CLOCK_REALTIME, 0);
    if (fd < 0) {
        _LOG_LAST_ERROR("timerfd_create failed(%s)", strerror(errno));
        return kSYSTEM_ERROR;
    }

    // 使用fcntl替代create的第2个参数仅做内核版本兼容性保证
    int32_t flags = fcntl(fd, F_GETFL, 0);
    flags < 0 ? flags = O_NONBLOCK : flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0) {
        close(fd);
        _LOG_LAST_ERROR("fcntl %d failed(%s)", fd, strerror(errno));
        return kSYSTEM_ERROR;
    }

    // 设置超时时间
    struct itimerspec timeout;
    timeout.it_value.tv_sec     = timeout_ms / 1000;
    timeout.it_value.tv_nsec    = (timeout_ms % 1000) * 1000 * 1000;
    timeout.it_interval.tv_sec  = timeout.it_value.tv_sec;
    timeout.it_interval.tv_nsec = timeout.it_value.tv_nsec;

    if (timerfd_settime(fd, 0, &timeout, NULL) < 0) {
        close(fd);
        _LOG_LAST_ERROR("timerfd_settime %d failed(%s)", fd, strerror(errno));
        return kSYSTEM_ERROR;
    }

    TimerItem* timeritem = new TimerItem();
    timeritem->fd = fd;
    timeritem->cb = cb;
    timeritem->id = m_timer_seqid;

    struct epoll_event event;
    event.data.ptr = timeritem;
    event.events   = EPOLLIN | EPOLLET;
    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        close(fd);
        delete timeritem;
        timeritem = NULL;
        _LOG_LAST_ERROR("epoll_ctl add %d failed(%s)", fd, strerror(errno));
        return kSYSTEM_ERROR;
    }

    m_timers[m_timer_seqid] = timeritem;

    return m_timer_seqid++;
}

int32_t FdTimer::StopTimer(int64_t timer_id) {
    cxx::unordered_map<int64_t, TimerItem*>::iterator it = m_timers.find(timer_id);
    if (m_timers.end() == it) {
        _LOG_LAST_ERROR("timer id %ld not exist", timer_id);
        return kTIMER_UNEXISTED;
    }

    int32_t fd = it->second->fd;
    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        _LOG_LAST_ERROR("epoll_ctl del %d failed(%s)", fd, strerror(errno));
    }

    close(fd);

    delete it->second;
    it->second = NULL;

    m_timers.erase(it);

    return 0;
}

int32_t FdTimer::Update() {
    const size_t MAX_EVENTS = 256;
    const uint32_t BUFF_LEN = 128;
    struct epoll_event events[MAX_EVENTS];

    int32_t num = epoll_wait(m_epoll_fd, events, std::min(MAX_EVENTS, m_timers.size()), 0);
    if (num <= 0) {
        return 0;
    }

    char buf[BUFF_LEN]   = {0};
    int64_t timerid      = 0;
    int32_t fd           = -1;
    int32_t ret          = 0;
    TimerItem* timeritem = NULL;

    for (int32_t i = 0; i < num; i++) {
        timeritem = static_cast<TimerItem*>(events[i].data.ptr);
        while (read(timeritem->fd, buf, BUFF_LEN) > 0) {} // NOLINT

        // 定时器可能在回调中被停掉，这里记录有效值
        timerid = timeritem->id;
        fd      = timeritem->fd;
        ret     = timeritem->cb();

        // 返回 <0 删除定时器，=0 继续，>0按新的超时时间重启定时器
        if (ret < 0) {
            StopTimer(timerid);
        } else if (ret > 0) {
            struct itimerspec timeout;
            timeout.it_value.tv_sec     = ret / 1000;
            timeout.it_value.tv_nsec    = (ret % 1000) * 1000 * 1000;
            timeout.it_interval.tv_sec  = timeout.it_value.tv_sec;
            timeout.it_interval.tv_nsec = timeout.it_value.tv_nsec;
            timerfd_settime(fd, 0, &timeout, NULL);
        }
    }

    return num;
}
#endif

SequenceTimer::SequenceTimer() {
    m_in_callback = false;
    m_timer_seqid   = 0;
    m_last_error[0] = 0;
}

SequenceTimer::~SequenceTimer() {
}

int64_t SequenceTimer::StartTimer(uint32_t timeout_ms, const TimeoutCallback& cb) {
    if (!cb || 0 == timeout_ms) {
        _LOG_LAST_ERROR("param is invalid: timeout_ms = %u, cb = %d", timeout_ms, (cb ? true : false));
        return kTIMER_INVALID_PARAM;
    }

    TimerItem* item = new TimerItem;
    item->id         = m_timer_seqid;
    item->timeout_ms = timeout_ms;
	item->start_time = TimeUtility::GetCurrentMS();
    item->cb         = cb;

	DbListItem& head = m_timer_lists[timeout_ms];
	if (head._next == NULL || head._prev == NULL) {
		db_list_init(&head);
	}
	db_list_add_tail(&head, &item->list_item);

    m_timers[m_timer_seqid] = item;

    return m_timer_seqid++;
}

int32_t SequenceTimer::StopTimer(int64_t timer_id) {
    if (m_in_callback) {
        _LOG_LAST_ERROR("timer in callback, can't stop");
        return kTIMER_IN_CALLBACK;
    }

    cxx::unordered_map<int64_t, TimerItem*>::iterator it = m_timers.find(timer_id);
    if (m_timers.end() == it) {
        _LOG_LAST_ERROR("timer id %ld not exist", timer_id);
        return kTIMER_UNEXISTED;
    }

	TimerItem* timer_item = it->second;
	db_list_del(&timer_item->list_item);
	delete timer_item;

    m_timers.erase(it);

    return 0;
}

int32_t SequenceTimer::ReStartTimer(int64_t timer_id) {
     if (m_in_callback) {
        _LOG_LAST_ERROR("timer in callback, can't restart");
        return kTIMER_IN_CALLBACK;
    }

	cxx::unordered_map<int64_t, TimerItem*>::iterator it = m_timers.find(timer_id);
    if (m_timers.end() == it) {
        _LOG_LAST_ERROR("timer id %ld not exist", timer_id);
        return kTIMER_UNEXISTED;
    }

    TimerItem* timer_item = it->second;
	timer_item->start_time = TimeUtility::GetCurrentMS();
	
	DbListItem& head = m_timer_lists[timer_item->timeout_ms];
	assert(head._next != NULL && head._prev != NULL);
	db_list_del(&timer_item->list_item);
	db_list_add_tail(&head, &timer_item->list_item);

    return 0;
}

int32_t SequenceTimer::Update() {
    int32_t num = 0;
    int64_t now = TimeUtility::GetCurrentMS();
    int32_t ret = 0;
    m_in_callback = true;

    cxx::unordered_map<uint32_t, DbListItem>::iterator it = m_timer_lists.begin();
    for (; it != m_timer_lists.end(); it++) {
		DbListItem& head = it->second;
		DbListItem* item = head._next;
		while (item != &head) {
			TimerItem* timer_item = container(TimerItem, list_item, item);
			assert(timer_item);
			if (timer_item->start_time + timer_item->timeout_ms > now) {
				break;
			}

			ret = timer_item->cb(timer_item->id);
            //用户在超时回调中可能stop/restart定时器，这里需要特殊处理
            
			// 返回 <0 删除定时器，=0 继续，>0按新的超时时间重启定时器
	        if (ret < 0) {
				db_list_del(item);
	            m_timers.erase(timer_item->id);
				delete item;
	        } else {
	            if (ret > 0) {
	                timer_item->timeout_ms = ret;
	            }
				timer_item->start_time = now;
				db_list_del(item);
				db_list_add_tail(&head, item);
	        }

			item = head._next;
			num++;
        }
    }

    m_in_callback = false;

    return num;
}

}  // namespace pebble

