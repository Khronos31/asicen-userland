// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_PREPARE_OPERATION_DIAGNOSTIC_H
#define ASICEN_USERLAND_PREPARE_OPERATION_DIAGNOSTIC_H

#if defined(DTV_USERLAND_DIAGNOSTICS)
#include <atomic>

namespace asicen {
namespace detail {

struct PrepareOperationContext {
    const char* phase = "unknown";
    const char* operation = "unknown";
    unsigned index = 0U;
    bool has_index = false;
};

inline thread_local PrepareOperationContext prepare_operation_context{};
inline std::atomic<bool> prepare_diagnostic_active{false};

class PrepareOperationScope {
public:
    PrepareOperationScope(const char* phase, const char* operation,
                          unsigned index = 0U, bool has_index = false) noexcept
        : previous_(prepare_operation_context)
    {
        if (phase != nullptr) prepare_operation_context.phase = phase;
        if (operation != nullptr) prepare_operation_context.operation = operation;
        if (has_index) {
            prepare_operation_context.index = index;
            prepare_operation_context.has_index = true;
        }
    }

    ~PrepareOperationScope() { prepare_operation_context = previous_; }

private:
    PrepareOperationContext previous_;
};

} // namespace detail
} // namespace asicen
#else
namespace asicen {
namespace detail {

class PrepareOperationScope {
public:
    PrepareOperationScope(const char*, const char*, unsigned = 0U,
                          bool = false) noexcept {}
};

} // namespace detail
} // namespace asicen
#endif

#endif
