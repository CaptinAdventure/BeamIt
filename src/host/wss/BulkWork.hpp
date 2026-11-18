#pragma once

#include "host/wss/ControlSession.hpp"

namespace beamit::host::wss {

// A caller awaits every operation before queuing the next frame. Exceptions are
// mapped to scoped codes here: filesystem exception text can contain private paths.
template<class T, class Function>
awaitable<std::expected<T, std::string>> bulk_disk(Server& server, Function function) {
    auto executor = co_await asio::this_coro::executor;
    co_return co_await asio::async_initiate<decltype(use_awaitable), void(std::expected<T, std::string>)>(
        [&server, executor, function = std::move(function)](auto handler) mutable {
            asio::post(server.bulk_worker,
                [executor, function = std::move(function), handler = std::move(handler)]() mutable {
                    std::expected<T, std::string> result = std::unexpected("storage_failed");
                    try { result = function(); }
                    catch (const ClipboardBulkStorageError& error) { result = std::unexpected(error.code()); }
                    catch (const std::exception&) {}
                    asio::post(executor, [handler = std::move(handler), result = std::move(result)]() mutable {
                        handler(std::move(result));
                    });
                });
        }, use_awaitable);
}

} // namespace beamit::host::wss
