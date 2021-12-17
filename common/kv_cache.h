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


#ifndef _PEBBLE_KV_CACHE_H
#define _PEBBLE_KV_CACHE_H

#include "common/platform.h"

namespace pebble {

/// @brief 基于kv的本地buff
class KVCache
{
public:
    KVCache();
    ~KVCache();

    /// @brief 初始化buff
    /// @param max_frame_num 最大缓存的key数
    /// @param total_mem_size 缓存的总内存大小
    /// @param block_size 缓存的块大小
    int32_t Init(uint32_t max_frame_num, uint32_t block_num, uint32_t block_size);

    /// @brief 写入缓存
    /// @param key 写入缓存的键值
    /// @param buff 写入缓存的内容指针
    /// @param length 写入消息的长度
    /// @param is_overwrite 是否覆盖写，否 - 相当于append追加写；是 - 覆盖写
    int32_t Put(uint64_t key, const char* buff, uint32_t length, bool is_overwrite = false);

    /// @brief 读出缓存
    /// @param key 读取缓存的键值
    /// @param buff 用于读取缓存的buff
    /// @param length 传入buff的长度，返回实际读取的长度
    /// @return >=0 返回读取的数据长度，不存在是返回0
    /// @note 如果buff的长度小于缓存的数据长度，则只读取buff长度部分，剩下的部分下次仍可继续读取
    int32_t Get(uint64_t key, char* buff, uint32_t length);

    /// @brief 窥视缓存
    /// @param key 窥视缓存的键值
    /// @param buff 用于窥视缓存的buff
    /// @param length 传入buff的长度，返回实际窥视的长度
    /// @return >=0 返回获取的数据长度，不存在是返回0
    /// @note 与Get不同，Peek不会删除缓存
    int32_t Peek(uint64_t key, char* buff, uint32_t length);

    /// @brief 获取指定key的缓存数据长度
    /// @return 返回缓存的数据长度
    int32_t GetSize(uint64_t key);

    /// @brief 销毁缓存
    /// @param key 缓存的键值
    int32_t Del(uint64_t key);

private:
    struct CacheBlockInfo
    {
        uint16_t _write_pos;    ///< 当前block中写的位置
        uint16_t _read_pos;     ///< 当前block中读的位置
        uint32_t _next_block;
    };
    struct CacheHeadInfo
    {
        CacheHeadInfo() : _first_block(-1), _last_block(-1) {}
        uint32_t _first_block;
        uint32_t _last_block;
    };

    uint32_t m_block_num;
    uint32_t m_block_size;
    uint32_t m_free_block_size;         ///< 空闲的存储块数量
    CacheHeadInfo m_free_block_head;    ///< 空闲的存储块头

    CacheBlockInfo*     m_block_infos;  ///< 存储块的信息
    char*               m_block_mem;    ///< 存储块的数据

    typedef cxx::unordered_map<uint64_t, CacheHeadInfo> CacheMap;
    typedef std::pair<uint64_t, CacheHeadInfo> CacheMapPair;
    CacheMap            m_caches;
};

} // namespace pebble

#endif // _PEBBLE_KV_CACHE_H
