/* cabbird/thread_local_value.hpp -- thread-local storage without a TLS directory.
 *
 * Ported from anomaly/include/anomaly/thread_local_value.hpp.
 *
 * WHY THIS EXISTS.  A manually mapped image cannot use `thread_local` / `__declspec(thread)`:
 * the PE's TLS directory needs the loader to allocate a per-thread block and fill
 * `_tls_index`, and there is no loader.  The mapper rejects an image that declares one
 * (the mapper refuses the image).  FLS needs none of that -- `FlsAlloc` carves a
 * slot out of the OS's own per-thread structure, so it works in a mapped image.
 *
 * WHAT IT DOES NOT FIX, and this distinction matters:
 *
 *   FlsAlloc-based storage  <- this file.  For state YOU declare.
 *   compiler-generated TLS  <- NOT fixable here.  MSVC emits direct `_tls_index` accesses
 *                              for function-local statics with non-trivial destructors
 *                              ("magic statics") and for `thread_local` variables.  Those
 *                              accesses cannot be redirected to FLS, so the only fix is to
 *                              not write them: use ThreadLocalObject, or move the value to
 *                              namespace scope (which is safe, because the mapper calls
 *                              the PE entry point -> _DllMainCRTStartup -> _initterm).
 *
 * So: `static std::mutex` inside a function is still wrong and will still fail the build.
 * A per-thread scratch buffer, a recursion guard, or a per-thread cache is what this is for.
 *
 * The property that makes this a real replacement rather than a leak: FlsAlloc takes a
 * destructor callback, and Windows invokes it when the THREAD exits.  A value stored by a
 * worker thread that dies is freed by that thread's own exit, not left behind.
 */
#pragma once

#include <Windows.h>

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace cabbird {

/* Per-thread heap-allocated instance of T.
 *
 * The value is created on first Get() on each thread, so a thread that never calls Get()
 * pays nothing.  Copy and move are deleted: a "copy" of this object would be a second FLS
 * slot, which is never what the caller means. */
template <typename T>
class ThreadLocalObject final {
public:
    /* Default: each thread gets a default-constructed T. */
    ThreadLocalObject()
        : slot_(FlsAlloc(&DestroyValue)), create_([] { return new T{}; }) {
        ThrowIfOutOfSlots();
    }

    /* Each thread's value is constructed from these arguments on first use.
     *
     * A HEAP-ALLOCATING FACTORY, not a prototype.  Two limitations of the obvious designs
     * are both avoided here, and both are real:
     *
     *   `T initial_` + `make_unique<T>(initial_)`   requires T copy-constructible.
     *   `T()` factory + `make_unique<T>(factory())` requires T move-constructible.
     *
     * Either way a per-thread arena, a mutex-guarded buffer, or anything else with a
     * deleted copy/move silently cannot be stored -- and the error surfaces deep inside
     * <memory> rather than at the declaration, which is how it was found.  Constructing
     * the object directly on the heap from the captured arguments needs neither: T only
     * has to be constructible from the arguments given.
     *
     * The cost is one std::function per ThreadLocalObject, allocated once per process. */
    template <typename... Args>
        requires(sizeof...(Args) > 0)
    explicit ThreadLocalObject(Args&&... args)
        : slot_(FlsAlloc(&DestroyValue)),
          create_([... captured = std::forward<Args>(args)]() mutable {
              return new T(std::move(captured)...);
          }) {
        ThrowIfOutOfSlots();
    }

    ~ThreadLocalObject() {
        if (slot_ == FLS_OUT_OF_INDEXES) {
            return;
        }
        // Frees THIS thread's value.  Other threads' values are freed by their own
        // thread-exit callbacks -- which is exactly why the callback exists.
        Clear();
        static_cast<void>(FlsFree(slot_));
    }

    ThreadLocalObject(const ThreadLocalObject&) = delete;
    ThreadLocalObject& operator=(const ThreadLocalObject&) = delete;

    /* Current thread's value, constructed from the captured arguments on first use. */
    [[nodiscard]] T& Get() {
        if (void* current = FlsGetValue(slot_); current != nullptr) {
            return *static_cast<T*>(current);
        }
        T* value = create_();
        if (FlsSetValue(slot_, value) == FALSE) {
            // The slot now owns nothing, so free it here.  Without this the failed store
            // leaks -- small, rare, and exactly the kind of leak that is never found.
            delete value;
            throw std::bad_alloc();
        }
        // Ownership moved to the slot; DestroyValue (on thread exit) or Clear() frees it.
        return *value;
    }

    /* True when the current thread already has a value (i.e. Get() would not construct). */
    [[nodiscard]] bool HasValue() const noexcept {
        return slot_ != FLS_OUT_OF_INDEXES && FlsGetValue(slot_) != nullptr;
    }

    /* Destroy the current thread's value now, without waiting for thread exit.
     *
     * The host uses this when it wants a thread's cached state gone while the thread is
     * still alive -- for example when a plugin is unloaded and its per-thread caches must
     * not outlive it. */
    void Clear() noexcept {
        if (slot_ == FLS_OUT_OF_INDEXES) {
            return;
        }
        if (void* current = FlsGetValue(slot_); current != nullptr) {
            // Order matters: null the slot BEFORE destroying, so a destructor that
            // re-enters Get() cannot find and return the object being destroyed.
            static_cast<void>(FlsSetValue(slot_, nullptr));
            DestroyValue(current);
        }
    }

private:
    static void NTAPI DestroyValue(void* value) noexcept {
        delete static_cast<T*>(value);
    }

    // Throwing beats returning a silently-broken object: FLS exhaustion means the process
    // is out of slots, and every later Get() would be wrong.
    void ThrowIfOutOfSlots() const {
        if (slot_ == FLS_OUT_OF_INDEXES) {
            throw std::bad_alloc();
        }
    }

    DWORD slot_{FLS_OUT_OF_INDEXES};
    /* Returns a heap-allocated T owned by the FLS slot.  A pointer rather than a T so that
     * constructing a thread's value never copies or moves one. */
    std::function<T*()> create_;
};

/* Per-thread scalar (integral, enum, or pointer) stored directly in the FLS slot.
 *
 * No allocation and no destructor callback, so this is the right choice for counters,
 * "am I inside this hook?" flags and pointer-sized handles.  The static_assert is the
 * point of the type: a larger T would be silently truncated through void*, and finding
 * that by debugging is far worse than finding it by compiling. */
template <typename T>
class ThreadLocalScalar final {
    static_assert(
        (std::is_integral_v<T> || std::is_enum_v<T> || std::is_pointer_v<T>) &&
            sizeof(T) <= sizeof(void*),
        "ThreadLocalScalar holds one pointer-sized value; use ThreadLocalObject for bigger "
        "or non-trivially-copyable types");

public:
    ThreadLocalScalar() : slot_(FlsAlloc(nullptr)) {
        if (slot_ == FLS_OUT_OF_INDEXES) {
            throw std::bad_alloc();
        }
    }

    ~ThreadLocalScalar() {
        if (slot_ != FLS_OUT_OF_INDEXES) {
            static_cast<void>(FlsFree(slot_));
        }
    }

    ThreadLocalScalar(const ThreadLocalScalar&) = delete;
    ThreadLocalScalar& operator=(const ThreadLocalScalar&) = delete;

    /* Zero for a thread that has never called Set().  FlsAlloc's nullptr initialiser makes
     * that the natural default rather than an uninitialised read. */
    [[nodiscard]] T Get() const noexcept {
        const auto value = reinterpret_cast<std::uintptr_t>(FlsGetValue(slot_));
        if constexpr (std::is_pointer_v<T>) {
            return reinterpret_cast<T>(value);
        } else {
            return static_cast<T>(value);
        }
    }

    void Set(T value) noexcept {
        std::uintptr_t encoded{};
        if constexpr (std::is_pointer_v<T>) {
            encoded = reinterpret_cast<std::uintptr_t>(value);
        } else {
            encoded = static_cast<std::uintptr_t>(value);
        }
        if (FlsSetValue(slot_, reinterpret_cast<void*>(encoded)) == FALSE) {
            // No exception here on purpose: Set is noexcept, and a failed FlsSetValue means
            // the slot is invalid, which is a programming error rather than a recoverable
            // condition.
            std::terminate();
        }
    }

    void Reset() noexcept { Set(T{}); }

private:
    DWORD slot_{FLS_OUT_OF_INDEXES};
};

}  // namespace cabbird
