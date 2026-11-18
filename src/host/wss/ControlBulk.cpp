#include "host/wss/ControlSession.hpp"
#include "host/wss/BulkWork.hpp"

#include <algorithm>

namespace beamit::host::wss {
namespace {
namespace bulk = net::clipboard_bulk;
using namespace std::chrono_literals;

void close_socket(const std::shared_ptr<BulkEntry>& entry) {
    if (!entry->socket) return;
    beast::error_code ignored;
    beast::get_lowest_layer(*entry->socket).close(ignored);
}

} // namespace

void ControlSession::bulk_negotiate(const json::object& message, const RequestId& id) {
    if (auth_generation_ != server_.auth_generation) {
        throw ProtocolError{"not_authenticated", "reconnect after authorization changed"};
    }
    if (required_integer(message, "version", 1, 1) != 1) return;
    bulk_negotiated_ = true;
    reply(id, {{"type", "clipboard_bulk_capabilities"}, {"version", 1},
        {"max_file_bytes", bulk::max_file_bytes}, {"max_roots", bulk::max_roots},
        {"max_nodes", bulk::max_nodes}, {"max_manifest_bytes", bulk::max_manifest_bytes},
        {"max_png_bytes", bulk::max_png_bytes}, {"max_chunk_bytes", bulk::max_chunk_size},
        {"max_png_dimension", bulk::max_png_dimension}, {"max_png_pixels", bulk::max_png_pixels},
        {"max_component_bytes", 255}, {"max_path_bytes", 4096},
        {"staging_quota_bytes", 8'000'000'000ULL}, {"scratch_bytes", 128 * 1024 * 1024}});
}

void ControlSession::bulk_begin(const json::object& message, const RequestId& id) {
    if (!bulk_negotiated_) throw ProtocolError{"not_ready", "negotiate clipboard bulk first"};
    if (auth_generation_ != server_.auth_generation) {
        throw ProtocolError{"not_authenticated", "reconnect after authorization changed"};
    }
    if (bulk_) throw ProtocolError{"limit_reached", "one clipboard transfer is already active"};
    if (!server_.clipboard && !server_.clipboard_publish) {
        throw ProtocolError{"clipboard_unavailable", "clipboard access is unavailable"};
    }
    const auto source = required_string(message, "source_snapshot_id");
    if (source.empty() || source.size() > 128) throw ProtocolError{"invalid_request", "invalid source snapshot"};
    const auto* field = message.if_contains("manifest");
    if (!field) throw ProtocolError{"invalid_request", "manifest is required"};
    const auto parsed = bulk::parse_manifest(*field, json::serialize(*field).size());
    if (!parsed) throw ProtocolError{"invalid_request", "invalid clipboard manifest"};
    if (std::ranges::count_if(parsed->nodes, [](const auto& node) {
            return node.kind == bulk::Node::Kind::Png;
        }) > 1) throw ProtocolError{"invalid_request", "a clipboard snapshot has at most one PNG"};
    auto entry = std::make_shared<BulkEntry>(bulk::issue_socket_credentials());
    entry->created = entry->progress = server_.bulk_clock();
    entry->manifest = *parsed;
    entry->source_snapshot = source;
    entry->destination_sequence = server_.clipboard_sequence();
    entry->metadata.expected_sequence = entry->destination_sequence;
    entry->metadata.publication_allowed = entry->allowed;
    std::size_t small_bytes{};
    for (const auto name : {"text", "html"}) {
        if (!message.contains(name)) continue;
        const auto text = required_string(message, name);
        small_bytes += text.size();
        if (std::string_view(name) == "text") entry->metadata.text = std::string(text);
        else entry->metadata.html = std::string(text);
    }
    if (small_bytes > protocol::max_clipboard_bytes) {
        throw ProtocolError{"invalid_request", "small clipboard formats exceed 10 MiB"};
    }
    if (const auto* sensitive = message.if_contains("sensitive")) {
        if (!sensitive->is_bool()) throw ProtocolError{"invalid_request", "sensitive must be a boolean"};
        entry->metadata.sensitive = sensitive->as_bool();
    }
    bulk_ = entry;
    entry->preparation_timer = std::make_unique<asio::steady_timer>(ws_->get_executor(), 9s);
    entry->preparation_timer->async_wait([weak = weak_from_this(), entry, id](beast::error_code error) {
        if (error) return;
        if (const auto self = weak.lock(); self && self->bulk_ == entry && !entry->ready) {
            self->send_error(id, "timeout", "clipboard storage preparation timed out");
            self->finish_bulk("timeout");
        }
    });
    schedule_bulk_timeout();
    asio::co_spawn(ws_->get_executor(), prepare_bulk(shared_from_this(), entry, id), asio::detached);
}

awaitable<void> ControlSession::prepare_bulk(std::shared_ptr<ControlSession> self,
                                             std::shared_ptr<BulkEntry> entry, RequestId id) {
    const auto configured_root = self->server_.bulk_staging_root;
    const auto write_check = self->server_.bulk_write_check;
    const auto result = co_await bulk_disk<bool>(self->server_, [entry, configured_root, write_check] {
        if (!entry->allowed->load()) return false;
        const auto root = configured_root.empty() ? clipboard_staging_root() : configured_root;
        entry->receiver = std::make_unique<ClipboardBulkReceiver>(
            root, entry->credentials.transfer_id_text, entry->manifest, "host", write_check);
        return true;
    });
    if (self->bulk_ != entry || entry->terminal) co_return;
    entry->preparation_timer->cancel();
    if (!result || !*result) {
        const auto code = result ? "storage_failed" : result.error();
        self->send_error(id, code, "staging quota, space or storage preparation failed");
        self->finish_bulk(code);
        co_return;
    }
    if (self->server_.clipboard_sequence() != entry->destination_sequence) {
        self->send_error(id, "stale_clipboard", "destination clipboard changed");
        self->finish_bulk("stale_clipboard");
        co_return;
    }
    entry->ready = true;
    entry->offered = entry->progress = self->server_.bulk_clock();
    self->reply(id, {{"type", "clipboard_bulk_offer"}, {"direction", "client_to_host"},
        {"transfer_id", entry->credentials.transfer_id_text}, {"source_snapshot_id", entry->source_snapshot},
        {"destination_sequence", entry->destination_sequence}, {"manifest", bulk::manifest_json(entry->manifest)},
        {"url", "wss://" + self->authority_ + "/bulk/" + entry->credentials.transfer_id_text +
                    "?token=" + entry->credentials.token_text}, {"expires_in_ms", 10000}});
}

std::shared_ptr<BulkEntry> ControlSession::claim_bulk(std::string_view id, std::string_view token) {
    if (!authenticated_ || closing_ || auth_generation_ != server_.auth_generation ||
        server_.active.lock().get() != this || !bulk_ || !bulk_->ready || bulk_->terminal ||
        id != bulk_->credentials.transfer_id_text || server_.bulk_clock() - bulk_->offered >= 10s ||
        bulk_->socket || !bulk_->token.claim(token)) return {};
    if (bulk_->download && server_.clipboard_sequence() != bulk_->source->sequence()) {
        finish_bulk("source_changed");
        return {};
    }
    return bulk_;
}

void ControlSession::attach_bulk(const std::shared_ptr<BulkEntry>& entry, std::shared_ptr<WebSocket> socket) {
    if (bulk_ != entry || entry->terminal || closing_ || auth_generation_ != server_.auth_generation) {
        beast::error_code ignored;
        beast::get_lowest_layer(*socket).close(ignored);
        return;
    }
    entry->socket = std::move(socket);
    entry->progress = server_.bulk_clock();
    if (entry->download) {
        asio::co_spawn(ws_->get_executor(), send_bulk(shared_from_this(), entry), asio::detached);
        asio::co_spawn(ws_->get_executor(), watch_download_socket(shared_from_this(), entry), asio::detached);
    } else asio::co_spawn(ws_->get_executor(), receive_bulk(shared_from_this(), entry), asio::detached);
}

awaitable<void> ControlSession::receive_bulk(std::shared_ptr<ControlSession> self,
                                             std::shared_ptr<BulkEntry> entry) {
    beast::flat_buffer buffer(bulk::frame_header_size + bulk::max_chunk_size);
    // Admission on both socket and disk stays bounded to a single 256 KiB chunk.
    // A one-MiB token bucket throttles payload before each disk admission.
    double tokens = bulk::max_queued_payload;
    auto replenished = Clock::now();
    try {
        while (!entry->terminal) {
            buffer.clear();
            co_await entry->socket->async_read(buffer, use_awaitable);
            if (entry->terminal) co_return;
            if (entry->ended || !entry->socket->got_binary()) {
                self->finish_bulk("invalid_frame");
                co_return;
            }
            const auto view = buffer.cdata();
            std::vector<std::byte> frame(static_cast<const std::byte*>(view.data()),
                                         static_cast<const std::byte*>(view.data()) + view.size());
            const auto header = bulk::decode_header(frame);
            if (!header) { self->finish_bulk("invalid_frame"); co_return; }
            const auto now = Clock::now();
            tokens = std::min<double>(bulk::max_queued_payload,
                tokens + std::chrono::duration<double>(now - replenished).count() * 8 * 1024 * 1024);
            replenished = now;
            if (tokens < header->payload_size) {
                asio::steady_timer throttle(co_await asio::this_coro::executor);
                throttle.expires_after(std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>((header->payload_size - tokens) / (8 * 1024 * 1024))));
                co_await throttle.async_wait(use_awaitable);
                tokens = 0;
                replenished = Clock::now();
            } else tokens -= header->payload_size;
            if (entry->terminal) co_return;
            const auto result = co_await bulk_disk<bool>(self->server_, [entry, frame = std::move(frame)] {
                if (!entry->allowed->load() || !entry->receiver) return false;
                const auto accepted = entry->receiver->accept(frame);
                if (!accepted) {
                    const auto& error = accepted.error();
                    throw ClipboardBulkStorageError(error == "digest_mismatch" || error == "invalid_png" ||
                        error == "storage_failed" || error == "disk_full" || error == "permission_denied" ?
                        error : "invalid_frame");
                }
                return true;
            });
            if (entry->terminal) co_return;
            if (!result || !*result) { self->finish_bulk(result ? "cancelled" : result.error()); co_return; }
            entry->progress = self->server_.bulk_clock();
            entry->received += header->payload_size;
            if (header->kind == bulk::FrameKind::End) {
                entry->ended = true;
                entry->completed_nodes = static_cast<std::uint32_t>(entry->manifest.nodes.size());
                self->publish_bulk();
            } else if (header->offset + header->payload_size == entry->manifest.nodes[header->node_index].size) {
                ++entry->completed_nodes;
            }
            if (entry->progress - entry->reported >= 250ms) {
                entry->reported = entry->progress;
                self->send({{"type", "clipboard_bulk_progress"}, {"transfer_id", entry->credentials.transfer_id_text},
                    {"phase", "receiving"}, {"completed_bytes", entry->received},
                    {"total_bytes", bulk::payload_bytes(entry->manifest)}, {"completed_nodes", entry->completed_nodes},
                    {"total_nodes", entry->manifest.nodes.size()}});
            }
        }
    } catch (const std::exception&) {
        if (!entry->terminal && !entry->ended) self->finish_bulk("interrupted");
    }
}

void ControlSession::bulk_commit(const json::object& message, const RequestId& id) {
    const auto transfer = required_string(message, "transfer_id");
    const auto source = required_string(message, "source_snapshot_id");
    if (transfer == last_bulk_id_ && source == last_bulk_source_) {
        reply(id, {{"type", "clipboard_bulk_committed"}, {"transfer_id", transfer}});
        return;
    }
    if (!bulk_ || bulk_->download || transfer != bulk_->credentials.transfer_id_text ||
        source != bulk_->source_snapshot) {
        if (bulk_) finish_bulk("snapshot_mismatch");
        throw ProtocolError{"invalid_request", "commit does not match the active transfer"};
    }
    bulk_->committed = true;
    reply(id, {{"type", "clipboard_bulk_committed"}, {"transfer_id", transfer}});
    publish_bulk();
}

void ControlSession::bulk_cancel(const json::object& message, const RequestId&) {
    if (!bulk_ || required_string(message, "transfer_id") != bulk_->credentials.transfer_id_text) {
        throw ProtocolError{"not_found", "unknown clipboard transfer"};
    }
    finish_bulk("cancelled", true);
}

void ControlSession::invalidate_bulk() {
    if (snapshot_job_) snapshot_job_->store(false);
    if (snapshot_timer_) snapshot_timer_->cancel();
    bulk_snapshot_.reset();
    bulk_snapshot_id_.clear();
    if (bulk_) finish_bulk("session_ended");
}

void ControlSession::finish_bulk(std::string_view code, bool cancelled, std::string_view detail, bool notify) {
    const auto entry = std::exchange(bulk_, {});
    if (!entry) return;
    entry->terminal = true;
    entry->allowed->store(false);
    if (entry->timer) entry->timer->cancel();
    if (entry->preparation_timer) entry->preparation_timer->cancel();
    close_socket(entry);
    json::object event{{"type", cancelled ? "clipboard_bulk_cancelled" : "clipboard_bulk_failed"},
                       {"transfer_id", entry->credentials.transfer_id_text}, {"code", code}};
    if (!detail.empty()) event["message"] = detail;
    if (notify) send(std::move(event));
    // A queued clipboard operation owns its preparation until its callback finishes.
    if (!entry->publishing) asio::post(server_.bulk_worker, [entry] {
        entry->receiver.reset();
        entry->spool.reset();
    });
}

void ControlSession::schedule_bulk_timeout() {
    const auto entry = bulk_;
    entry->timer = std::make_unique<asio::steady_timer>(ws_->get_executor(), 1s);
    entry->timer->async_wait([weak = weak_from_this(), entry](beast::error_code error) {
        const auto self = weak.lock();
        if (error || !self || self->bulk_ != entry) return;
        const auto now = self->server_.bulk_clock();
        if (entry->download && !entry->ready) {
            const auto tick = entry->prepared_tick.load();
            if (tick) entry->progress = Clock::time_point(Clock::duration(tick));
            const auto bytes = entry->prepared_bytes.load();
            if (bytes != entry->received && now - entry->reported >= 250ms) {
                entry->received = bytes;
                entry->reported = now;
                self->send({{"type", "clipboard_bulk_progress"}, {"transfer_id", entry->credentials.transfer_id_text},
                    {"phase", "preparing"}, {"completed_bytes", bytes},
                    {"total_bytes", net::clipboard_bulk::payload_bytes(entry->manifest)},
                    {"completed_nodes", 0}, {"total_nodes", entry->manifest.nodes.size()}});
            }
        }
        if (now - entry->created >= 30min || now - entry->progress >= 30s ||
            (entry->ready && !entry->token.used() && now - entry->offered >= 10s)) {
            self->finish_bulk("timeout");
        } else self->schedule_bulk_timeout();
    });
}

void ControlSession::publish_bulk() {
    if (!bulk_ || !bulk_->ended || !bulk_->committed || bulk_->publishing || bulk_->terminal) return;
    bulk_->publishing = true;
    asio::co_spawn(ws_->get_executor(), publish_bulk_async(shared_from_this(), bulk_), asio::detached);
}

awaitable<void> ControlSession::publish_bulk_async(std::shared_ptr<ControlSession> self,
                                                   std::shared_ptr<BulkEntry> entry) {
    const auto prepared = co_await bulk_disk<ClipboardWrite>(self->server_, [entry] {
        if (!entry->allowed->load()) throw std::runtime_error("cancelled");
        const auto roots = entry->receiver->promote();
        if (!roots) throw std::runtime_error("promotion failed");
        auto write = entry->metadata;
        write.files = *roots;
        const auto png = entry->receiver->prepared_png();
        if (!png) throw std::runtime_error("PNG preparation failed");
        if (!png->empty()) write.png = *png;
        return write;
    });
    if (!prepared || entry->terminal || self->server_.clipboard_sequence() != entry->destination_sequence) {
        if (!entry->terminal) self->finish_bulk(prepared ? "stale_clipboard" : "storage_failed");
        asio::post(self->server_.bulk_worker, [entry] { entry->receiver.reset(); });
        co_return;
    }
    if (self->server_.bulk_clock() - entry->reported >= 250ms) {
        entry->reported = self->server_.bulk_clock();
        self->send({{"type", "clipboard_bulk_progress"}, {"transfer_id", entry->credentials.transfer_id_text},
                {"phase", "publishing"}, {"completed_bytes", entry->received},
                {"total_bytes", bulk::payload_bytes(entry->manifest)},
                {"completed_nodes", entry->manifest.nodes.size()}, {"total_nodes", entry->manifest.nodes.size()}});
    }
    const auto executor = co_await asio::this_coro::executor;
    auto result = co_await asio::async_initiate<decltype(use_awaitable), void(ClipboardWriteResult)>(
        [self, entry, executor, write = *prepared](auto handler) mutable {
            // std::function must remain copyable across the native clipboard callback.
            auto completion = std::make_shared<decltype(handler)>(std::move(handler));
            auto done = [entry, executor, completion](ClipboardWriteResult result) mutable {
                if (result.error_code.empty() || result.retain_files) entry->receiver->published();
                asio::post(executor, [completion, result = std::move(result)]() mutable {
                    (*completion)(std::move(result));
                });
            };
            if (self->server_.clipboard_publish) self->server_.clipboard_publish(std::move(write), std::move(done));
            else self->server_.clipboard->write(std::move(write), std::move(done));
        }, use_awaitable);
    const bool success = result.error_code.empty();
    // Retain every path handed successfully to the OS even if cancellation raced the callback.
    const auto cleaned = co_await bulk_disk<bool>(self->server_, [entry, success] {
        if (success) entry->receiver->published();
        entry->receiver.reset();
        return true;
    });
    if (!cleaned && !entry->terminal) {
        self->finish_bulk("storage_failed");
        co_return;
    }
    if (entry->terminal) co_return;
    if (!success) {
        self->finish_bulk(result.error_code == "stale_clipboard" ? "stale_clipboard" : "clipboard_unavailable",
                          false, result.error);
        co_return;
    }
    entry->terminal = true;
    entry->allowed->store(false);
    if (entry->timer) entry->timer->cancel();
    close_socket(entry);
    self->last_bulk_id_ = entry->credentials.transfer_id_text;
    self->last_bulk_source_ = entry->source_snapshot;
    self->bulk_.reset();
    self->send({{"type", "clipboard_bulk_complete"}, {"transfer_id", entry->credentials.transfer_id_text},
                {"sequence", result.sequence}});
}

} // namespace beamit::host::wss
