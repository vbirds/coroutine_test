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


#include <stdlib.h>
#include <string.h>
#include <algorithm>

#include "kv_cache.h"
#include "log.h"

namespace pebble {


static const uint64_t __stl_prime_list[28] = {
	53ul,         97ul,         193ul,       389ul,       769ul,
	1543ul,       3079ul,       6151ul,      12289ul,     24593ul,
	49157ul,      98317ul,      196613ul,    393241ul,    786433ul,
	1572869ul,    3145739ul,    6291469ul,   12582917ul,  25165843ul,
	50331653ul,   100663319ul,  201326611ul, 402653189ul, 805306457ul,
	1610612741ul, 3221225473ul, 4294967291ul
};

inline uint64_t __stl_next_prime(uint64_t __n) {
	const uint64_t* __first = __stl_prime_list;
	const uint64_t* __last = __stl_prime_list + 28;
	const uint64_t* pos = std::lower_bound(__first, __last, __n);
	return pos == __last ? *(__last - 1) : *pos;
}


KVCache::KVCache()
    :   m_block_num(15000), m_block_size(512), m_free_block_size(0),
        m_block_infos(NULL), m_block_mem(NULL)
{
}

KVCache::~KVCache()
{
    if (NULL != m_block_infos)
    {
        delete [] m_block_infos;
        m_block_infos = NULL;
    }
    if (NULL != m_block_mem)
    {
        delete [] m_block_mem;
        m_block_mem = NULL;
    }
    m_caches.clear();
}

int32_t KVCache::Init(uint32_t max_frame_num, uint32_t block_num, uint32_t block_size)
{
    size_t hash_size = __stl_next_prime(static_cast<size_t>(max_frame_num));
    m_caches.rehash(hash_size); // 访问非常频繁，稀疏桶，用空间换时间

    m_block_num = (block_num > 0 ? block_num : m_block_num);
    m_block_size = (block_size > 0 ? block_size : m_block_size);

    m_block_infos = new CacheBlockInfo[block_num];
    m_block_mem = new char[m_block_num * m_block_size];

    // 初始化block infos
    for (uint32_t idx = 0 ; idx < m_block_num ; ++idx)
    {
        m_block_infos[idx]._write_pos = 0;
        m_block_infos[idx]._read_pos = 0;
        m_block_infos[idx]._next_block = idx + 1;
    }
    m_block_infos[m_block_num - 1]._next_block = UINT32_MAX;

    m_free_block_head._first_block = 0;
    m_free_block_head._last_block = m_block_num - 1;
    m_free_block_size = m_block_num;
    return 0;
}

int32_t KVCache::Put(uint64_t key, const char* buff, uint32_t length, bool is_overwrite)
{
    // 至少保留一个block做为free_block
    uint32_t need_block_num = length / m_block_size + ((length % m_block_size) == 0 ? 0 : 1);
    if (m_free_block_size <= need_block_num + 10 || NULL == m_block_infos)
    {
        PLOG_ERROR_N_EVERY_SECOND(1, "Put failed in block not enough, need %u remain %u",
            need_block_num, m_free_block_size);
        return -1;
    }
    if (true == is_overwrite)
    {
        Del(key);
    }

    std::pair<CacheMap::iterator, bool> result;
    result = m_caches.insert(CacheMapPair(key, CacheHeadInfo()));
    // 新插入，分配一块内存
    if (true == result.second)
    {
        m_free_block_size--;
        uint32_t malloc_block_id = m_free_block_head._first_block;
        m_free_block_head._first_block = m_block_infos[malloc_block_id]._next_block;

        m_block_infos[malloc_block_id]._next_block = UINT32_MAX;
        m_block_infos[malloc_block_id]._write_pos = 0;
        m_block_infos[malloc_block_id]._read_pos = 0;
        result.first->second._first_block = malloc_block_id;
        result.first->second._last_block = malloc_block_id;
    }

    // 写入数据
    uint32_t has_write = 0;
    uint32_t last_block_id = result.first->second._last_block;
    while (has_write < length)
    {
        uint32_t buff_sz = m_block_size - m_block_infos[last_block_id]._write_pos;
        uint32_t remain_sz = length - has_write;
        buff_sz = (buff_sz > remain_sz ? remain_sz : buff_sz);

        char* cur_write = m_block_mem + last_block_id * m_block_size
                          + m_block_infos[last_block_id]._write_pos;
        memcpy(cur_write, buff + has_write, buff_sz);
        m_block_infos[last_block_id]._write_pos += buff_sz;
        has_write += buff_sz;

        if (has_write < length)
        {
            m_free_block_size--;
            m_block_infos[last_block_id]._next_block = m_free_block_head._first_block;
            last_block_id = m_free_block_head._first_block;
            m_free_block_head._first_block = m_block_infos[last_block_id]._next_block;
            m_block_infos[last_block_id]._next_block = UINT32_MAX;
            m_block_infos[last_block_id]._write_pos = 0;
            m_block_infos[last_block_id]._read_pos = 0;
        }
    }
    result.first->second._last_block = last_block_id;

    return 0;
}

int32_t KVCache::Get(uint64_t key, char* buff, uint32_t length)
{
    CacheMap::iterator it = m_caches.find(key);
    if (NULL == buff || 0 == length || it == m_caches.end())
    {
        return 0;
    }

    uint32_t has_read = 0;
    uint32_t first_block_id = it->second._first_block;
    while (first_block_id < m_block_num && has_read < length)
    {
        uint32_t buff_sz = length - has_read;
        uint32_t remain_sz = m_block_infos[first_block_id]._write_pos
                             - m_block_infos[first_block_id]._read_pos;
        buff_sz = (buff_sz < remain_sz ? buff_sz : remain_sz);

        char* cur_read = m_block_mem + first_block_id * m_block_size
                         + m_block_infos[first_block_id]._read_pos;
        memcpy(buff + has_read, cur_read, buff_sz);
        m_block_infos[first_block_id]._read_pos += buff_sz;
        has_read += buff_sz;

        // block读取了所有的数据了
        if (buff_sz == remain_sz)
        {
            m_block_infos[m_free_block_head._last_block]._next_block = first_block_id;
            m_free_block_head._last_block = first_block_id;
            first_block_id = m_block_infos[first_block_id]._next_block;
            m_block_infos[m_free_block_head._last_block]._next_block = UINT32_MAX;
            m_free_block_size++;
        }
    }
    it->second._first_block = first_block_id;
    // 所有的数据都读取完了，删除
    if (first_block_id >= m_block_num)
    {
        m_caches.erase(it);
    }
    return static_cast<int32_t>(has_read);
}

int32_t KVCache::Peek(uint64_t key, char* buff, uint32_t length)
{
    CacheMap::iterator it = m_caches.find(key);
    if (NULL == buff || 0 == length || it == m_caches.end())
    {
        return 0;
    }

    uint32_t has_read = 0;
    uint32_t first_block_id = it->second._first_block;
    while (first_block_id < m_block_num && has_read < length)
    {
        uint32_t buff_sz = length - has_read;
        uint32_t remain_sz = m_block_infos[first_block_id]._write_pos
            - m_block_infos[first_block_id]._read_pos;
        buff_sz = (buff_sz < remain_sz ? buff_sz : remain_sz);

        char* cur_read = m_block_mem + first_block_id * m_block_size
            + m_block_infos[first_block_id]._read_pos;
        memcpy(buff + has_read, cur_read, buff_sz);
        has_read += buff_sz;

        // block读取了所有的数据了
        if (buff_sz == remain_sz)
        {
            first_block_id = m_block_infos[first_block_id]._next_block;
        }
    }

    return static_cast<int32_t>(has_read);
}

int32_t KVCache::GetSize(uint64_t key)
{
    CacheMap::iterator it = m_caches.find(key);
    if (it == m_caches.end())
    {
        return 0;
    }

    uint32_t total_len = 0;
    uint32_t first_block_id = it->second._first_block;
    while (first_block_id < m_block_num)
    {
        uint32_t remain_sz = m_block_infos[first_block_id]._write_pos
            - m_block_infos[first_block_id]._read_pos;

        total_len += remain_sz;
        first_block_id = m_block_infos[first_block_id]._next_block;
    }
    return total_len;
}

int32_t KVCache::Del(uint64_t key)
{
    CacheMap::iterator it = m_caches.find(key);
    if (it == m_caches.end())
    {
        return -1;
    }

    m_block_infos[m_free_block_head._last_block]._next_block = it->second._first_block;
    m_free_block_head._last_block = it->second._last_block;
    m_block_infos[m_free_block_head._last_block]._next_block = UINT32_MAX;

    uint32_t first_block_id = it->second._first_block;
    while (first_block_id < m_block_num)
    {
        first_block_id = m_block_infos[first_block_id]._next_block;
        m_free_block_size++;
    }
    m_caches.erase(it);
    return 0;
}

} // namespace pebble
