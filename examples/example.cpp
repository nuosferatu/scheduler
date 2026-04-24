// DataHub.h 的基本功能测试 + 过程日志
//
// 覆盖点：
//   1. DataPool 的 acquire / release / 耗尽行为
//   2. DataBuffer 的 push / pop / get_by_id
//   3. DataBuffer 容量超限时的 FIFO 淘汰
//   4. 自定义 copy_func 的调用
//   5. 多线程下的 get_by_id 并发
//   6. DataHub 的注册 / 查询 / 覆盖 / 类型不匹配
//
// 日志约定：
//   [RUN ] / [ OK ] / [FAIL] —— 测试用例状态
//   [STEP] —— 当前操作的语义描述
//   [DATA] —— 读/写的具体数值
//   [STAT] —— buffer/池当前状态（size、剩余槽位等）
//   [INFO] —— 其他说明性信息
//
// 注意：DataHub / DataBuffer 在错误分支里自带 printf 打印（例如
//       "DataBuffer: buffer is empty"、"DataHub: ... not found" 等），
//       下面某些用例会故意触发它们。凡是故意触发的用例都会先打 [INFO]，
//       看到这些 printf 属于预期输出，不代表测试失败。

#include "DataHub.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using scheduler::DataBuffer;
using scheduler::DataPool;
using scheduler::DataHub;
using scheduler::DataTypeId;
using scheduler::DataTypeInfo;
using scheduler::frame_id_t;

namespace {

struct Pose {
    double x;
    double y;
    double yaw;
};

std::ostream& operator<<(std::ostream& os, const Pose& p) {
    os << "{x=" << p.x << ", y=" << p.y << ", yaw=" << p.yaw << "}";
    return os;
}

int g_passed = 0;
int g_failed = 0;

// 统一日志前缀，使输出带层次感
#define LOG_STEP(msg) std::cout << "  [STEP] " << msg << std::endl
#define LOG_DATA(msg) std::cout << "  [DATA] " << msg << std::endl
#define LOG_STAT(msg) std::cout << "  [STAT] " << msg << std::endl
#define LOG_INFO(msg) std::cout << "  [INFO] " << msg << std::endl

#define EXPECT(cond)                                                         \
    do {                                                                     \
        if (cond) {                                                          \
            ++g_passed;                                                      \
        } else {                                                             \
            ++g_failed;                                                      \
            std::cerr << "  [FAIL] " << __func__ << " line " << __LINE__     \
                      << ": " #cond << std::endl;                            \
        }                                                                    \
    } while (0)

// RAII 帮手：进入时打 [RUN ], 离开时打 [ OK ]/[FAIL]
struct CaseScope {
    const char* name;
    int failed_before;
    CaseScope(const char* n) : name(n), failed_before(g_failed) {
        std::cout << "\n[RUN ] " << name << std::endl;
    }
    ~CaseScope() {
        if (g_failed == failed_before) {
            std::cout << "[ OK ] " << name << std::endl;
        } else {
            std::cout << "[FAIL] " << name
                      << " (" << (g_failed - failed_before) << " failure(s))"
                      << std::endl;
        }
    }
};

// -----------------------------------------------------------------------------
// DataPool
// -----------------------------------------------------------------------------

void test_data_pool_basic() {
    CaseScope _("test_data_pool_basic");

    const size_t pool_size = 2;
    const size_t elem_size = sizeof(int);
    LOG_STEP("构造 DataPool<int>(pool_size=" << pool_size
             << ", elem_size=" << elem_size << ")");
    DataPool<int> pool(pool_size, elem_size);

    LOG_STEP("连续 acquire_ptr 两次");
    int* p1 = pool.acquire_ptr();
    int* p2 = pool.acquire_ptr();
    LOG_DATA("p1=" << static_cast<void*>(p1) << ", p2=" << static_cast<void*>(p2));
    EXPECT(p1 != nullptr);
    EXPECT(p2 != nullptr);
    EXPECT(p1 != p2);

    LOG_STEP("写入 *p1=42, *p2=100 并校验");
    *p1 = 42;
    *p2 = 100;
    LOG_DATA("*p1=" << *p1 << ", *p2=" << *p2);
    EXPECT(*p1 == 42);
    EXPECT(*p2 == 100);

    LOG_STEP("释放 p1, p2 后再次 acquire, 应得到被清零的内存");
    pool.release_ptr(p1);
    pool.release_ptr(p2);
    int* p3 = pool.acquire_ptr();
    LOG_DATA("p3=" << static_cast<void*>(p3) << ", *p3=" << (p3 ? *p3 : -1));
    EXPECT(p3 != nullptr);
    EXPECT(*p3 == 0);
    pool.release_ptr(p3);
}

void test_data_pool_exhaust() {
    CaseScope _("test_data_pool_exhaust");

    const size_t N = 3;
    LOG_STEP("构造 DataPool<int>(pool_size=" << N << ")");
    DataPool<int> pool(N, sizeof(int));

    std::vector<int*> ptrs;
    LOG_STEP("连续 acquire " << N << " 次，直到池耗尽");
    for (size_t i = 0; i < N; ++i) {
        int* p = pool.acquire_ptr();
        LOG_DATA("acquire[" << i << "] -> " << static_cast<void*>(p));
        EXPECT(p != nullptr);
        ptrs.push_back(p);
    }

    LOG_STEP("再 acquire 一次，预期返回 nullptr");
    int* overflow = pool.acquire_ptr();
    LOG_DATA("acquire[overflow] -> " << static_cast<void*>(overflow));
    EXPECT(overflow == nullptr);

    LOG_STEP("释放一个指针后再 acquire，应能成功");
    pool.release_ptr(ptrs.back());
    ptrs.pop_back();
    int* p = pool.acquire_ptr();
    LOG_DATA("after release, acquire -> " << static_cast<void*>(p));
    EXPECT(p != nullptr);
    ptrs.push_back(p);

    for (auto* q : ptrs) pool.release_ptr(q);
    LOG_INFO("已释放全部指针");
}

// -----------------------------------------------------------------------------
// DataBuffer
// -----------------------------------------------------------------------------

void test_data_buffer_push_pop() {
    CaseScope _("test_data_buffer_push_pop");

    const size_t capacity = 4;
    LOG_STEP("构造 DataBuffer<int>(capacity=" << capacity << ")");
    DataBuffer<int> buf(capacity, sizeof(int), nullptr);
    LOG_STAT("size=" << buf.size());
    EXPECT(buf.size() == 0);

    LOG_STEP("push (id=1,v=10), (id=2,v=20), (id=3,v=30)");
    EXPECT(buf.push(1, 10));
    EXPECT(buf.push(2, 20));
    EXPECT(buf.push(3, 30));
    LOG_STAT("size=" << buf.size());
    EXPECT(buf.size() == 3);

    int v = 0;
    LOG_STEP("pop(v), 预期拿到最早写入的 10");
    EXPECT(buf.pop(v));
    LOG_DATA("popped v=" << v);
    EXPECT(v == 10);
    LOG_STAT("size=" << buf.size());
    EXPECT(buf.size() == 2);

    LOG_STEP("pop() 不取值, 仅弹出队首");
    EXPECT(buf.pop());
    LOG_STAT("size=" << buf.size());
    EXPECT(buf.size() == 1);

    LOG_STEP("pop(v), 预期拿到 30");
    EXPECT(buf.pop(v));
    LOG_DATA("popped v=" << v);
    EXPECT(v == 30);
    LOG_STAT("size=" << buf.size());
    EXPECT(buf.size() == 0);

    LOG_INFO("下一步会触发 DataBuffer 内部 printf: \"buffer is empty\"");
    LOG_STEP("对空 buffer 继续 pop, 预期 false");
    EXPECT(!buf.pop());
    int dummy = 0;
    EXPECT(!buf.pop(dummy));
}

void test_data_buffer_get_by_id() {
    CaseScope _("test_data_buffer_get_by_id");

    LOG_STEP("构造 DataBuffer<int>(capacity=4), push 三条");
    DataBuffer<int> buf(4, sizeof(int), nullptr);
    buf.push(100, 111);
    buf.push(200, 222);
    buf.push(300, 333);
    LOG_STAT("size=" << buf.size());

    int v = 0;
    LOG_STEP("get_by_id(200)");
    EXPECT(buf.get_by_id(200, v));
    LOG_DATA("value=" << v);
    EXPECT(v == 222);

    LOG_STEP("get_by_id(100)");
    EXPECT(buf.get_by_id(100, v));
    LOG_DATA("value=" << v);
    EXPECT(v == 111);

    LOG_INFO("下一步会触发 DataBuffer 内部 printf: \"id 999 not found\"");
    LOG_STEP("get_by_id(999) 不存在, 预期 false");
    bool got = buf.get_by_id(999, v);
    LOG_DATA("returned " << std::boolalpha << got);
    EXPECT(!got);
}

void test_data_buffer_capacity_evict() {
    CaseScope _("test_data_buffer_capacity_evict");

    const size_t capacity = 2;
    LOG_STEP("构造 DataBuffer<int>(capacity=" << capacity << ")");
    DataBuffer<int> buf(capacity, sizeof(int), nullptr);

    LOG_STEP("push id=1,2 填满");
    buf.push(1, 10);
    buf.push(2, 20);
    LOG_STAT("size=" << buf.size());
    EXPECT(buf.size() == 2);

    LOG_STEP("push id=3 触发淘汰, 最早的 id=1 应被踢出");
    buf.push(3, 30);
    LOG_STAT("size=" << buf.size());
    EXPECT(buf.size() <= 2);

    LOG_INFO("下一步会触发 DataBuffer 内部 printf: \"id 1 not found\"");
    int v = 0;
    bool got1 = buf.get_by_id(1, v);
    LOG_DATA("get_by_id(1) -> " << std::boolalpha << got1);
    EXPECT(!got1);

    EXPECT(buf.get_by_id(2, v));
    LOG_DATA("get_by_id(2) -> value=" << v);
    EXPECT(v == 20);

    EXPECT(buf.get_by_id(3, v));
    LOG_DATA("get_by_id(3) -> value=" << v);
    EXPECT(v == 30);
}

void test_data_buffer_custom_copy_func() {
    CaseScope _("test_data_buffer_custom_copy_func");

    std::atomic<int> copy_count{0};
    auto copy_func = [&copy_count](void* dst, const void* src) {
        std::memcpy(dst, src, sizeof(Pose));
        copy_count.fetch_add(1, std::memory_order_relaxed);
    };
    LOG_STEP("构造 DataBuffer<Pose>(capacity=2) 并带自定义 copy_func");
    DataBuffer<Pose> buf(2, sizeof(Pose), copy_func);

    Pose p1{1.0, 2.0, 0.3};
    Pose p2{4.0, 5.0, 0.6};
    LOG_STEP("push id=1 pose=" << p1);
    buf.push(1, p1);
    LOG_STEP("push id=2 pose=" << p2);
    buf.push(2, p2);
    LOG_STAT("copy_count=" << copy_count.load() << " (期望 2)");

    Pose out{};
    LOG_STEP("get_by_id(1) 读出, 每次读都会触发一次 copy_func");
    EXPECT(buf.get_by_id(1, out));
    LOG_DATA("got pose=" << out);
    EXPECT(out.x == 1.0 && out.y == 2.0 && out.yaw == 0.3);

    LOG_STEP("pop(out) 再拿一次最旧的");
    EXPECT(buf.pop(out));
    LOG_DATA("popped pose=" << out);
    EXPECT(out.x == 1.0);

    LOG_STAT("copy_count=" << copy_count.load() << " (期望 4)");
    EXPECT(copy_count.load() == 4);
}

void test_data_buffer_concurrent_get() {
    CaseScope _("test_data_buffer_concurrent_get");

    const size_t capacity = 1024;
    const frame_id_t n = 200;
    const int rounds = 500;
    const int thread_num = 4;

    LOG_STEP("构造 DataBuffer<int>(capacity=" << capacity << ") 并预写 "
             << n << " 条数据");
    DataBuffer<int> buf(capacity, sizeof(int), nullptr);
    for (frame_id_t i = 1; i <= n; ++i) {
        buf.push(i, static_cast<int>(i * 2));
    }
    LOG_STAT("size=" << buf.size());

    std::atomic<int> hit{0};
    std::atomic<int> miss{0};
    auto worker = [&](int tid) {
        for (int round = 0; round < rounds; ++round) {
            for (frame_id_t i = 1; i <= n; ++i) {
                int v = 0;
                if (buf.get_by_id(i, v)) {
                    if (v == static_cast<int>(i * 2)) {
                        hit.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    miss.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        std::ostringstream oss;
        oss << "thread#" << tid << " done";
        LOG_INFO(oss.str());
    };

    LOG_STEP("启动 " << thread_num << " 个线程, 每线程执行 "
             << rounds << " 轮 x " << n << " 次 get_by_id");
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ths;
    for (int i = 0; i < thread_num; ++i) {
        ths.emplace_back(worker, i);
    }
    for (auto& t : ths) t.join();
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    const int expected = thread_num * rounds * static_cast<int>(n);
    LOG_STAT("hit=" << hit.load() << ", miss=" << miss.load()
             << ", expected_hit=" << expected
             << ", elapsed=" << ms << " ms");
    EXPECT(miss.load() == 0);
    EXPECT(hit.load() == expected);
}

// -----------------------------------------------------------------------------
// DataHub
// -----------------------------------------------------------------------------

void test_data_hub_basic() {
    CaseScope _("test_data_hub_basic");

    DataHub hub;
    LOG_STEP("hub.add_data_buffer<int>({LEFT_IMAGE, cap=4, elem=sizeof(int), copy=null})");
    hub.add_data_buffer<int>(
        DataTypeInfo{DataTypeId::LEFT_IMAGE, /*capacity=*/4,
                     /*elem_size=*/sizeof(int), /*copy_func=*/nullptr});

    LOG_STEP("hub.get_data_buffer<int>(LEFT_IMAGE)");
    auto* buf = hub.get_data_buffer<int>(DataTypeId::LEFT_IMAGE);
    LOG_DATA("buf ptr=" << static_cast<void*>(buf));
    EXPECT(buf != nullptr);

    LOG_STEP("通过 hub 拿到的 buffer 进行 push/get");
    EXPECT(buf->push(1, 42));
    EXPECT(buf->push(2, 99));
    int v = 0;
    EXPECT(buf->get_by_id(1, v));
    LOG_DATA("get_by_id(1) -> " << v);
    EXPECT(v == 42);
    EXPECT(buf->get_by_id(2, v));
    LOG_DATA("get_by_id(2) -> " << v);
    EXPECT(v == 99);
}

void test_data_hub_multi_type() {
    CaseScope _("test_data_hub_multi_type");

    DataHub hub;
    LOG_STEP("同时注册 int(LEFT_IMAGE) 和 Pose(POINT_CLOUD)");
    hub.add_data_buffer<int>(
        DataTypeInfo{DataTypeId::LEFT_IMAGE, 4, sizeof(int), nullptr});
    hub.add_data_buffer<Pose>(
        DataTypeInfo{DataTypeId::POINT_CLOUD, 4, sizeof(Pose), nullptr});

    auto* ibuf = hub.get_data_buffer<int>(DataTypeId::LEFT_IMAGE);
    auto* pbuf = hub.get_data_buffer<Pose>(DataTypeId::POINT_CLOUD);
    LOG_DATA("ibuf=" << static_cast<void*>(ibuf)
             << ", pbuf=" << static_cast<void*>(pbuf));
    EXPECT(ibuf != nullptr);
    EXPECT(pbuf != nullptr);

    LOG_STEP("ibuf.push(1, 7), pbuf.push(1, {1.1,2.2,3.3})");
    ibuf->push(1, 7);
    pbuf->push(1, Pose{1.1, 2.2, 3.3});

    int iv = 0;
    Pose pv{};
    EXPECT(ibuf->get_by_id(1, iv));
    LOG_DATA("ibuf.get_by_id(1) -> " << iv);
    EXPECT(iv == 7);

    EXPECT(pbuf->get_by_id(1, pv));
    LOG_DATA("pbuf.get_by_id(1) -> " << pv);
    EXPECT(pv.x == 1.1 && pv.y == 2.2 && pv.yaw == 3.3);
}

void test_data_hub_miss_and_type_mismatch() {
    CaseScope _("test_data_hub_miss_and_type_mismatch");

    DataHub hub;
    hub.add_data_buffer<int>(
        DataTypeInfo{DataTypeId::LEFT_IMAGE, 4, sizeof(int), nullptr});

    LOG_INFO("下一步会触发 DataHub 内部 printf: "
             "\"data_type_id `<RIGHT_IMAGE 枚举值>` not found\"");
    LOG_STEP("查询未注册的 id RIGHT_IMAGE");
    auto* p1 = hub.get_data_buffer<int>(DataTypeId::RIGHT_IMAGE);
    LOG_DATA("returned ptr=" << static_cast<void*>(p1));
    EXPECT(p1 == nullptr);

    LOG_INFO("下一步会触发 DataHub 内部 printf: "
             "\"data_type_id `<LEFT_IMAGE 枚举值>` type mismatch\"");
    LOG_STEP("已注册但类型不匹配: 用 Pose 去拿 LEFT_IMAGE, 预期 nullptr 而非异常");
    auto* p2 = hub.get_data_buffer<Pose>(DataTypeId::LEFT_IMAGE);
    LOG_DATA("returned ptr=" << static_cast<void*>(p2));
    EXPECT(p2 == nullptr);
}

void test_data_hub_duplicate_register() {
    CaseScope _("test_data_hub_duplicate_register");

    DataHub hub;
    LOG_STEP("第一次注册 LEFT_IMAGE, capacity=2, 并 push 一条数据");
    hub.add_data_buffer<int>(
        DataTypeInfo{DataTypeId::LEFT_IMAGE, 2, sizeof(int), nullptr});
    auto* first_buf = hub.get_data_buffer<int>(DataTypeId::LEFT_IMAGE);
    LOG_DATA("first_buf ptr=" << static_cast<void*>(first_buf));
    EXPECT(first_buf != nullptr);
    EXPECT(first_buf->push(1, 111));
    LOG_STAT("first_buf.size()=" << first_buf->size());

    LOG_INFO("下一步会触发 DataHub 内部 printf: "
             "\"data_type_id `<LEFT_IMAGE 枚举值>` already exists\"");
    LOG_STEP("再次注册同 id(LEFT_IMAGE), capacity=4, 预期被拒绝, 原 buffer 保留");
    hub.add_data_buffer<int>(
        DataTypeInfo{DataTypeId::LEFT_IMAGE, 4, sizeof(int), nullptr});

    auto* second_buf = hub.get_data_buffer<int>(DataTypeId::LEFT_IMAGE);
    LOG_DATA("second_buf ptr=" << static_cast<void*>(second_buf));
    EXPECT(second_buf != nullptr);
    EXPECT(second_buf == first_buf);          // 指针应与第一次相同
    LOG_STAT("second_buf.size()=" << second_buf->size()
             << " (期望仍为 1, 原数据未丢)");
    EXPECT(second_buf->size() == 1);

    LOG_STEP("通过第二次取回的指针校验: 原 push 的数据 (id=1, v=111) 仍在");
    int v = 0;
    EXPECT(second_buf->get_by_id(1, v));
    LOG_DATA("get_by_id(1) -> " << v);
    EXPECT(v == 111);
}

}  // namespace

int main() {
    std::cout << "==== DataHub.h 功能测试 ====" << std::endl;

    auto t_start = std::chrono::steady_clock::now();

    test_data_pool_basic();
    test_data_pool_exhaust();
    test_data_buffer_push_pop();
    test_data_buffer_get_by_id();
    test_data_buffer_capacity_evict();
    test_data_buffer_custom_copy_func();
    test_data_buffer_concurrent_get();
    test_data_hub_basic();
    test_data_hub_multi_type();
    test_data_hub_miss_and_type_mismatch();
    test_data_hub_duplicate_register();

    auto t_end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_end - t_start).count();

    std::cout << "\n==== Summary ====\n";
    std::cout << "  passed : " << g_passed << "\n";
    std::cout << "  failed : " << g_failed << "\n";
    std::cout << "  elapsed: " << ms << " ms\n";
    std::cout << "  result : "
              << (g_failed == 0 ? "ALL PASS" : "SOME FAILED") << "\n";
    return g_failed == 0 ? 0 : 1;
}
