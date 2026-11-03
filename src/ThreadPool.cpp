#include "citius/ThreadPool.hpp"
#include "citius/Logger.hpp"
#include <exception>
#include <format>
#include <stdexcept>
#include <utility>

namespace citius {

ThreadPool::ThreadPool(std::size_t threads, std::size_t max_queue) : m_max_queue(max_queue) {
    if (threads == 0 || max_queue == 0) {
        throw std::invalid_argument("ThreadPool: threads e max_queue devem ser maiores que zero.");
    }
    m_workers.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        m_workers.emplace_back([this] { worker_loop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard const lock{m_mutex};
        m_stopping = true;
    }
    m_wake.notify_all();
    m_workers.clear(); // jthread faz join no destrutor
}

bool ThreadPool::submit(Task task) {
    {
        std::lock_guard const lock{m_mutex};
        if (m_stopping || m_queue.size() >= m_max_queue) {
            return false;
        }
        m_queue.push(std::move(task));
    }
    // fora do lock: o worker acordado nao precisa esperar o mutex que ainda seguramos
    m_wake.notify_one();
    return true;
}

void ThreadPool::worker_loop() {
    while (true) {
        Task task;
        {
            std::unique_lock lock{m_mutex};
            // o predicado protege contra "acordar sem motivo" (spurious wakeup) e contra
            // notificacoes perdidas: se a tarefa chegou antes de dormirmos, nem dormimos
            m_wake.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty()) {
                return; // encerrando e a fila ja foi esvaziada
            }
            task = std::move(m_queue.front());
            m_queue.pop();
        }

        // executa fora do lock, senao so uma tarefa rodaria por vez
        try {
            task();
        } catch (const std::exception& e) {
            log_error(std::format("Tarefa do ThreadPool lancou excecao: {}", e.what()));
        } catch (...) {
            log_error("Tarefa do ThreadPool lancou excecao desconhecida.");
        }
    }
}

} // namespace citius
