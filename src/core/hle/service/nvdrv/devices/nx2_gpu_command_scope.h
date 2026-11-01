// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace Service::Nvidia::Devices {

class Nx2GpuCommandScope {
public:
    template <typename Function>
    static auto Execute(Function&& function) {
        Nx2GpuCommandScope scope;
        return scope.Invoke(std::forward<Function>(function));
    }

    template <typename GPU, typename Function>
    static auto ExecuteOnGpu(GPU& gpu, Function&& function) {
        if (current) {
            throw std::logic_error{"GPU ownership boundary cannot nest in a command scope"};
        }
        std::unique_lock publication{publication_mutex};
        using Result = std::invoke_result_t<Function>;
        auto task = std::make_shared<std::packaged_task<Result()>>([&]() -> Result {
            Nx2GpuCommandScope scope{false};
            scope.submit = [](std::function<void()>&& work) { work(); };
            return scope.Invoke(std::forward<Function>(function));
        });
        auto result = task->get_future();
        gpu.RunInOrder([task = std::move(task)] { (*task)(); });
        return result.get();
    }

    Nx2GpuCommandScope(const Nx2GpuCommandScope&) = delete;
    Nx2GpuCommandScope& operator=(const Nx2GpuCommandScope&) = delete;

    template <typename GPU>
    static void Run(GPU& gpu, std::function<void()>&& command) {
        if (!current) {
            gpu.RunInOrder(std::move(command));
            return;
        }
        auto& scope = *current->root;
        if (!scope.submit) {
            scope.submit = [&gpu](std::function<void()>&& work) {
                gpu.RunInOrder(std::move(work));
            };
        }
        scope.commands->push_back(std::move(command));
    }

private:
    template <typename Function>
    auto Invoke(Function&& function) {
        try {
            if constexpr (std::is_void_v<std::invoke_result_t<Function>>) {
                function();
                Publish();
            } else {
                auto result = function();
                Publish();
                return result;
            }
        } catch (...) {
            Publish();
            throw;
        }
    }

    explicit Nx2GpuCommandScope(bool acquire_publication = true)
        : lock{publication_mutex, std::defer_lock}, previous{current},
          root{previous ? previous->root : this} {
        if (!previous) {
            if (acquire_publication) {
                lock.lock();
            }
            commands = std::make_shared<std::vector<std::function<void()>>>();
            batch = [work = commands] {
                for (const auto& command : *work) {
                    command();
                }
            };
        }
        current = this;
    }

    ~Nx2GpuCommandScope() {
        current = previous;
    }

    void Publish() {
        if (previous || published) {
            return;
        }
        published = true;
        current = previous;
        if (!commands->empty()) {
            submit(std::move(batch));
        }
    }

    inline static std::mutex publication_mutex;
    inline static thread_local Nx2GpuCommandScope* current{};
    std::unique_lock<std::mutex> lock;
    Nx2GpuCommandScope* previous;
    Nx2GpuCommandScope* root;
    bool published{};
    std::function<void(std::function<void()>&&)> submit;
    std::shared_ptr<std::vector<std::function<void()>>> commands;
    std::function<void()> batch;
};

} // namespace Service::Nvidia::Devices
