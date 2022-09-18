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

#ifndef BTHREAD_WORK_STEALING_QUEUE_H
#define BTHREAD_WORK_STEALING_QUEUE_H

#include "butil/macros.h"
#include "butil/atomicops.h"
#include "butil/logging.h"

// [_top, ..., _bottom)
// 总结 push、pop 是同一个线程操作，不需要任何内存序进行保护即可完美运行
// steal 一般是其他线程操作，因此需要一定的内存序确保 （push，steal） 以及 （pop，steal） 之间的可见性
// 其中（push，steal）最复杂

// 1. push 操作
// push 操作只会由当前 worker 发起，不和 push、steal 产生竞争关系。
// 所以获取 top 和 bottom 也是最宽松的，此时只需要判断队列是否满即可。
// push 从 _bottom 处插入资源即可
// _bottom.store(b + 1, butil::memory_order_release); 
// release 是确保 steal 操作对 _buffer[b & (_capacity - 1)] = x; 的可见性

// 2. pop 操作
// pop 操作只会由当前 worker 发起，不和 push 产生竞争关系，可能会和（多个）steal 产生竞争关系
// 首先利用 relax 快速判断队列是否为空，此时的 _bottom 肯定是 push 操作的最新值（单线程操作）
// 然后尝试将 --bottom，假装消费一个资源
// 此时需要将 --bottom 通过内存屏障的方式告知其他 steal，让其他 steal 立即意识到 bottom 已经修改了
// 然后获取最新的 top
// a. 如果没有资源，恢复 _bottom
// b. 如果有很多资源，直接消费
// c. 如果只有一个资源，pop 变成 steal 的方式进行竞争，并恢复 _bottom

// 3. steal
// 首先利用 memory_order_acquire 确保当前线程对 buffer 的可见性
// 然后利用一个 memory_order_seq_cst 确保见到最新的 bottom（和 pop 的 memory_order_seq_cst 进行搭配）
// 并尝试 CAS 修改 top 值
// 如果 CAS 成功，内存序为 memory_order_seq_cst，确保 pop 处获取到的是最新的 top

namespace bthread {

template <typename T>
class WorkStealingQueue {
public:
    WorkStealingQueue()
        : _bottom(1)
        , _capacity(0)
        , _buffer(NULL)
        , _top(1) {
    }

    ~WorkStealingQueue() {
        delete [] _buffer;
        _buffer = NULL;
    }

    int init(size_t capacity) {
        if (_capacity != 0) {
            LOG(ERROR) << "Already initialized";
            return -1;
        }
        if (capacity == 0) {
            LOG(ERROR) << "Invalid capacity=" << capacity;
            return -1;
        }
        // 必须是 2 的整数倍，快速取余
        if (capacity & (capacity - 1)) {
            LOG(ERROR) << "Invalid capacity=" << capacity
                       << " which must be power of 2";
            return -1;
        }
        _buffer = new(std::nothrow) T[capacity];
        if (NULL == _buffer) {
            return -1;
        }
        _capacity = capacity;
        return 0;
    }

    // 从队尾插入数据
    // Push an item into the queue.
    // Returns true on pushed.
    // May run in parallel with steal().
    // Never run in parallel with pop() or another push().
    bool push(const T& x) {
        // 由于 push 和 pop 只会由当前的 worker 调用，不会有任何的竞争关系
        // 说实在 top 没必要使用 memory_order_acquire，不需要同步的地方
        const size_t b = _bottom.load(butil::memory_order_relaxed);
        const size_t t = _top.load(butil::memory_order_acquire);
        // 检查是否已满
        if (b >= t + _capacity) { // Full queue.
            return false;
        }
        // 直接存放即可
        _buffer[b & (_capacity - 1)] = x;
        // release 确保 push、steal 读到 _bottem 最新值时，_buffer 也已经更新到最新值了
        _bottom.store(b + 1, butil::memory_order_release);
        return true;
    }

    // 从队尾弹出数据
    // 方式为：修改 bottom 的值，但是 steal 会修改 top 的值，所以需要检查
    // Pop an item from the queue.
    // Returns true on popped and the item is written to `val'.
    // May run in parallel with steal().
    // Never run in parallel with push() or another pop().
    bool pop(T* val) {
        // 快速检查队列是否为空
        const size_t b = _bottom.load(butil::memory_order_relaxed);
        size_t t = _top.load(butil::memory_order_relaxed);
        if (t >= b) {
            // fast check since we call pop() in each sched.
            // Stale _top which is smaller should not enter this branch.
            return false;
        }
        // 尝试消费一个数据
        const size_t newb = b - 1;
        _bottom.store(newb, butil::memory_order_relaxed);
        // 理解这里的内存屏障很重要
        // 因为没有修改 _buffer，所以 _bottom 不需要使用任何的内存序
        // 但是需要告知其他的线程，_bottom 已经修改了，内存屏障 memory_order_seq_cst 有这个功能
        // 同时内存屏障 memory_order_seq_cst 可以有效阻止 _top.load 重排到 _bottom.store
        // LOC.1
        butil::atomic_thread_fence(butil::memory_order_seq_cst);
        // LOC.2 
        t = _top.load(butil::memory_order_relaxed);
        // 如果没有数据了，说明被 steal 偷走了，恢复 bottom 并返回 false
        if (t > newb) {
            // _bottom 只有 push 和 pop 修改，push 和 pop 不会同时执行，所以可直接恢复
            _bottom.store(b, butil::memory_order_relaxed);
            return false;
        }
        *val = _buffer[newb & (_capacity - 1)];
        // 不是最后一个资源
        if (t != newb) {
            return true;
        }
        // 最后一个资源，需要和 steal 竞争，竞争的手段为 steal 的方式
        // 并恢复 _bottom 的值
        // Single last element, compete with steal()
        const bool popped = _top.compare_exchange_strong(
            t, t + 1, butil::memory_order_seq_cst, butil::memory_order_relaxed);
        _bottom.store(b, butil::memory_order_relaxed);
        return popped;
    }

    // Steal one item from the queue.
    // Returns true on stolen.
    // May run in parallel with push() pop() or another steal().
    bool steal(T* val) {
        // 这里全部使用 memory_order_acquire
        // 因为 pop、steal 可能会修改 top、buttom 和 buffer，确保可见性
        size_t t = _top.load(butil::memory_order_acquire);
        size_t b = _bottom.load(butil::memory_order_acquire);
        if (t >= b) {
            // Permit false negative for performance considerations.
            return false;
        }
        do {
            // 内存屏障 memory_order_seq_cst 保证 _bottom 是最新的
            // 和 pop LOC.1 处代码进行搭配
            butil::atomic_thread_fence(butil::memory_order_seq_cst);
            b = _bottom.load(butil::memory_order_acquire);
            if (t >= b) {
                return false;
            }
            *val = _buffer[t & (_capacity - 1)];
        } while (!_top.compare_exchange_strong(t, t + 1,
                                               butil::memory_order_seq_cst,
                                               butil::memory_order_relaxed));
        // CAS  
        // 成功时的内存序为 memory_order_seq_cst，为了和 pop LOC.2 处代码进行搭配
        // 失败时内存序为 memory_order_relaxed
                                               butil::memory_order_relaxed)); c
                                               butil::memory_order_relaxed));
        return true;
    }

    size_t volatile_size() const {
        const size_t b = _bottom.load(butil::memory_order_relaxed);
        const size_t t = _top.load(butil::memory_order_relaxed);
        return (b <= t ? 0 : (b - t));
    }

    size_t capacity() const { return _capacity; }

private:
    // Copying a concurrent structure makes no sense.
    DISALLOW_COPY_AND_ASSIGN(WorkStealingQueue);

    butil::atomic<size_t> _bottom;
    size_t _capacity;
    T* _buffer;
    butil::atomic<size_t> BAIDU_CACHELINE_ALIGNMENT _top;
};

}  // namespace bthread

#endif  // BTHREAD_WORK_STEALING_QUEUE_H
