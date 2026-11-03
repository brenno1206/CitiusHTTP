#pragma once
#include "citius/HttpRequest.hpp"
#include "citius/HttpResponse.hpp"
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace citius {

/// Escolhe quem responde a cada requisicao: um handler registrado para (metodo, caminho) ou,
/// quando nenhum caminho casa, o handler de reserva (fallback).
///
/// O casamento e exato (sem parametros como "/users/:id"), sem diferenciar nada alem do que
/// esta escrito: "/ping" nao casa com "/ping/" nem com "/PING". A query string do alvo
/// ("/ping?x=1") e ignorada ao casar, mas o handler recebe a requisicao original.
///
/// Nao e thread-safe para registrar rotas enquanto route() roda; registre tudo antes de servir.
class Router {
public:
    /// Funcao que transforma uma requisicao em resposta. Pode ser lambda, funcao ou functor.
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    /// Registra @p handler para GET @p path. Registrar de novo o mesmo par substitui o anterior.
    /// @throws std::invalid_argument se @p path nao comecar com '/', tiver '?' ou @p handler for vazio.
    void get(std::string_view path, Handler handler) { add(HttpMethod::GET, path, std::move(handler)); }
    void post(std::string_view path, Handler handler) { add(HttpMethod::POST, path, std::move(handler)); }
    void put(std::string_view path, Handler handler) { add(HttpMethod::PUT, path, std::move(handler)); }
    /// "delete" e palavra reservada do C++, por isso o nome e del().
    void del(std::string_view path, Handler handler) { add(HttpMethod::DELETE, path, std::move(handler)); }

    /// Registra @p handler para @p method e @p path. Mesmas regras e excecoes de get().
    void add(HttpMethod method, std::string_view path, Handler handler);

    /// Define quem atende os caminhos sem nenhuma rota (ex.: arquivos estaticos).
    /// @throws std::invalid_argument se @p handler for vazio.
    void set_fallback(Handler handler);

    /// Atende @p request:
    /// - caminho e metodo registrados: devolve o que o handler devolver;
    /// - caminho registrado, mas so para outros metodos: 405 com o cabecalho Allow
    ///   (o fallback nao e consultado: o recurso existe);
    /// - caminho sem rota nenhuma: fallback, ou 404 se nao houver.
    /// Excecoes lancadas pelo handler passam adiante; quem chama decide o que fazer.
    [[nodiscard]] HttpResponse route(const HttpRequest& request) const;

private:
    using MethodHandlers = std::map<HttpMethod, Handler>;

    std::map<std::string, MethodHandlers, std::less<>> m_routes; ///< caminho -> handlers por metodo
    Handler m_fallback;                                          ///< vazio = responder 404
};

} // namespace citius
