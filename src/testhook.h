#pragma once

#include <QMutex>

#include <functional>
#include <memory>
#include <utility>

// A step tests insert into work that runs on another thread. Whoever runs
// it calls its own copy, taken under the lock, so a hook replaced while
// another thread runs it, or waits in it, lives until that call returns.
template <typename... Args>
class TestHook {
public:
    void set(std::function<void(Args...)> hook)
    {
        auto replacement = hook ? std::make_shared<const std::function<void(Args...)>>(std::move(hook)) : nullptr;
        // The hook it replaces goes after the lock, once no call holds it.
        std::shared_ptr<const std::function<void(Args...)>> replaced;
        QMutexLocker lock(&m_lock);
        replaced = std::exchange(m_hook, std::move(replacement));
    }

    void operator()(Args... args) const
    {
        std::shared_ptr<const std::function<void(Args...)>> hook;
        {
            QMutexLocker lock(&m_lock);
            hook = m_hook;
        }
        if (hook) (*hook)(args...);
    }

private:
    mutable QMutex m_lock;
    std::shared_ptr<const std::function<void(Args...)>> m_hook;
};
