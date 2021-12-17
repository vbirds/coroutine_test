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


#ifndef _PEBBLE_COMMON_DB_LIST_H_
#define _PEBBLE_COMMON_DB_LIST_H_

#include <stdio.h>

namespace pebble {

typedef struct DbListItem {
    struct DbListItem* _prev;
    struct DbListItem* _next;

	DbListItem() { _prev = _next = NULL; }
} DbListItem;

//初始化一个链表
void db_list_init(DbListItem* head);

//添加节点到结尾
void db_list_add_tail(DbListItem* head, DbListItem* item);

//删除节点
void db_list_del(DbListItem* item);

} // namespace pebble

#endif // _PEBBLE_COMMON_DB_LIST_H_