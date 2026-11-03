#pragma once
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace citius {

/// Grupo fixo de threads que executam tarefas tiradas de uma fila limitada.
///
/// Quem chama submit() nao espera a tarefa: ela entra na fila e a primeira thread livre a executa.
/// Com a fila vazia as threads dormem (condition variable), sem gastar CPU.
/// O destrutor para de aceitar tarefas, deixa as que ja estao na fila terminarem e faz join.
///
/// Nao e copiavel nem movivel (as threads guardam referencia para o proprio pool).
class ThreadPool {
public:
    /// Tarefa a executar. Precisa ser copiavel (exigencia do std::function); para carregar um
    /// recurso move-only (ex.: Socket) guarde-o num std::shared_ptr.
    using Task = std::function<void()>;

    /// @param threads quantas threads criar (> 0).
    /// @param max_queue quantas tarefas podem esperar na fila (> 0); tarefas em execucao nao contam.
    /// @throws std::invalid_argument se @p threads ou @p max_queue for zero.
    ThreadPool(std::size_t threads, std::size_t max_queue);

    /// Termina as tarefas ja enfileiradas e espera as threads acabarem.
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// Enfileira @p task. Pode ser chamado de varias threads ao mesmo tempo.
    /// Uma excecao lancada pela tarefa e registrada em stderr e nao derruba a thread.
    /// @return false (e a tarefa nao roda) se a fila estiver cheia ou o pool estiver encerrando.
    [[nodiscard]] bool submit(Task task);

private:
    void worker_loop();

    std::mutex m_mutex;                  ///< protege m_queue e m_stopping
    std::condition_variable m_wake;      ///< acorda um worker quando chega tarefa ou o pool encerra
    std::queue<Task> m_queue;
    std::size_t m_max_queue;
    bool m_stopping = false;
    std::vector<std::jthread> m_workers; ///< por ultimo: sao destruidas (join) antes do mutex e da fila
};

} // namespace citius
