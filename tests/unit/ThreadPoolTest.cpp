#include "citius/ThreadPool.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>
#include <vector>

using citius::ThreadPool;

TEST(ThreadPoolTest, ExecutaTodasAsTarefas) {
    std::atomic<int> contador = 0;
    {
        ThreadPool pool{4, 1000};
        for (int i = 0; i < 100; ++i) {
            EXPECT_TRUE(pool.submit([&] { ++contador; }));
        }
    } // o destrutor termina a fila antes de retornar

    EXPECT_EQ(contador, 100);
}

TEST(ThreadPoolTest, DestrutorEsperaTarefasQueAindaEstaoNaFila) {
    std::atomic<int> concluidas = 0;
    {
        ThreadPool pool{1, 10}; // uma thread so: as outras tarefas ficam esperando na fila
        for (int i = 0; i < 5; ++i) {
            EXPECT_TRUE(pool.submit([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ++concluidas;
            }));
        }
    }

    EXPECT_EQ(concluidas, 5);
}

TEST(ThreadPoolTest, TarefasRodamEmParaleloDeVerdade) {
    constexpr int kThreads = 4;
    std::atomic<int> chegaram = 0;
    std::atomic<int> todas_chegaram = 0;
    {
        ThreadPool pool{kThreads, 10};
        for (int i = 0; i < kThreads; ++i) {
            EXPECT_TRUE(pool.submit([&] {
                ++chegaram;
                // so termina se as outras tarefas estiverem rodando ao mesmo tempo (com prazo para nao travar)
                auto prazo = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (chegaram < kThreads && std::chrono::steady_clock::now() < prazo) {
                    std::this_thread::yield();
                }
                if (chegaram == kThreads) {
                    ++todas_chegaram;
                }
            }));
        }
    }

    EXPECT_EQ(todas_chegaram, kThreads);
}

TEST(ThreadPoolTest, RejeitaTarefaQuandoAFilaEstaCheia) {
    std::promise<void> iniciou;
    std::promise<void> liberar;
    std::shared_future<void> liberar_futuro = liberar.get_future().share();
    std::atomic<int> executadas = 0;
    {
        ThreadPool pool{1, 1};

        // ocupa a unica thread; esperamos ela comecar para a fila ficar vazia de verdade
        EXPECT_TRUE(pool.submit([&] {
            iniciou.set_value();
            liberar_futuro.wait();
            ++executadas;
        }));
        iniciou.get_future().wait();

        EXPECT_TRUE(pool.submit([&] { ++executadas; }));   // ocupa a unica vaga da fila
        EXPECT_FALSE(pool.submit([&] { ++executadas; }));  // fila cheia: recusada

        liberar.set_value();
    }

    EXPECT_EQ(executadas, 2); // a recusada nunca roda
}

TEST(ThreadPoolTest, ExcecaoNaTarefaNaoMataAThread) {
    std::atomic<bool> rodou_depois = false;
    {
        ThreadPool pool{1, 10};
        EXPECT_TRUE(pool.submit([] { throw std::runtime_error("bug na tarefa"); }));
        EXPECT_TRUE(pool.submit([&] { rodou_depois = true; }));
    }

    EXPECT_TRUE(rodou_depois);
}

TEST(ThreadPoolTest, VariasThreadsPodemSubmeterAoMesmoTempo) {
    std::atomic<int> contador = 0;
    {
        ThreadPool pool{4, 10000};
        std::vector<std::thread> produtores;
        for (int p = 0; p < 4; ++p) {
            produtores.emplace_back([&] {
                for (int i = 0; i < 250; ++i) {
                    EXPECT_TRUE(pool.submit([&] { ++contador; }));
                }
            });
        }
        for (auto& produtor : produtores) {
            produtor.join();
        }
    }

    EXPECT_EQ(contador, 1000);
}

TEST(ThreadPoolTest, ConfiguracaoInvalidaLancaExcecao) {
    EXPECT_THROW(ThreadPool(0, 10), std::invalid_argument);
    EXPECT_THROW(ThreadPool(2, 0), std::invalid_argument);
}
