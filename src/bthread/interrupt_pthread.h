// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// bthread - A M:N threading library to make applications more concurrent.

// Date: Tue Jul 10 17:40:58 CST 2012

#ifndef BTHREAD_INTERRUPT_PTHREAD_H
#define BTHREAD_INTERRUPT_PTHREAD_H

#include <pthread.h>

namespace bthread {

// TaskControl stop_and_join 的时候会调用这个函数
// 用于唤醒所有的 worker
// 比如有些 worker 陷入 sys_call，使用 kill 可将他们唤醒
// Make blocking ops in the pthread returns -1 and EINTR.
// Returns what pthread_kill returns.
int interrupt_pthread(pthread_t th);

}  // namespace bthread

#endif // BTHREAD_INTERRUPT_PTHREAD_H
