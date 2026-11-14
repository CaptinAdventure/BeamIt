#include "host/wss/BulkWork.hpp"

#include <algorithm>

namespace beamit::host::wss {
namespace {
namespace bulk = net::clipboard_bulk;
using namespace std::chrono_literals;

// Measure live application payload ownership, including throttle waits, and the
// actual async_write lifetime. Destruction balances both on cancel or exception.
struct SendActivity {
    std::function<void(std::ptrdiff_t, int)> observer;
    std::ptrdiff_t bytes;
    bool writing{};
    SendActivity(Server& server, std::size_t size)
        : observer(server.bulk_send_activity), bytes(static_cast<std::ptrdiff_t>(size)) {
        if (observer) observer(bytes, 0);
    }
    void begin_write() { writing = true; if (observer) observer(0, 1); }
    void end_write() { writing = false; if (observer) observer(0, -1); }
    ~SendActivity() { if (observer) observer(-bytes, writing ? -1 : 0); }
};

std::string_view omission_name(ClipboardFormat format) {
    switch (format) {
    case clipboard_text: return "text";
    case clipboard_html: return "html";
    case clipboard_image: return "image";
    case clipboard_files: return "files";
    default: return "unknown";
    }
}

} // namespace

void ControlSession::read_clipboard(unsigned formats, bool binary, ClipboardMonitor::ReadCallback done) {
    if (server_.clipboard_read) server_.clipboard_read(formats, binary, std::move(done));
    else if (server_.clipboard) {
        if (binary) server_.clipboard->read_bulk(formats, std::move(done));
        else server_.clipboard->read(formats, protocol::max_clipboard_bytes, std::move(done));
    } else done({.error = "clipboard access is unavailable"});
}

void ControlSession::get_bulk_clipboard(unsigned formats, RequestId id, std::string reconfirm_transfer) {
    if (auth_generation_ != server_.auth_generation) {
        throw ProtocolError{"not_authenticated", "reconnect after authorization changed"};
    }
    if (bulk_) throw ProtocolError{"limit_reached", "one clipboard transfer is already active"};
    if (snapshot_abort_) snapshot_abort_();
    if (snapshot_timer_) snapshot_timer_->cancel();
    bulk_snapshot_.reset();
    bulk_snapshot_id_.clear();
    auto allowed = snapshot_job_ = std::make_shared<std::atomic_bool>(true);
    snapshot_abort_ = [weak = weak_from_this(), allowed, id, reconfirm_transfer] {
        const auto self = weak.lock();
        if (!self || !allowed->exchange(false)) return;
        if (self->snapshot_timer_) self->snapshot_timer_->cancel();
        if (reconfirm_transfer.empty()) self->send_error(id, "source_changed", "clipboard preflight invalidated");
        else self->send({{"type", "clipboard_bulk_failed"}, {"transfer_id", reconfirm_transfer},
                         {"code", "source_changed"}});
    };
    snapshot_timer_ = std::make_shared<asio::steady_timer>(ws_->get_executor(), 9s);
    snapshot_timer_->async_wait([weak = weak_from_this(), allowed, id, reconfirm_transfer](beast::error_code error) {
        const auto self = weak.lock();
        if (error || !self || !allowed->exchange(false)) return;
        if (reconfirm_transfer.empty()) self->send_error(id, "timeout", "clipboard preflight timed out");
        else self->send({{"type", "clipboard_bulk_failed"}, {"transfer_id", reconfirm_transfer}, {"code", "timeout"}});
    });
    read_clipboard(formats, true,
        [weak = weak_from_this(), allowed, id, reconfirm_transfer = std::move(reconfirm_transfer)](
            ClipboardReadResult result) mutable {
            const auto self = weak.lock();
            if (!self) return;
            asio::post(self->ws_->get_executor(),
                [self, allowed, id, reconfirm_transfer = std::move(reconfirm_transfer),
                 result = std::move(result)]() mutable {
                    if (!allowed->load() || self->closing_) return;
                    if (!result.content) {
                        allowed->store(false);
                        self->snapshot_timer_->cancel();
                        if (reconfirm_transfer.empty()) {
                            self->send_error(id, "clipboard_unavailable", "clipboard read failed");
                        } else self->send({{"type", "clipboard_bulk_failed"},
                            {"transfer_id", reconfirm_transfer}, {"code", "clipboard_unavailable"}});
                        return;
                    }
                    asio::co_spawn(self->ws_->get_executor(), snapshot_bulk(self, std::move(*result.content),
                        allowed, id, std::move(reconfirm_transfer)), asio::detached);
                });
        });
}

awaitable<void> ControlSession::snapshot_bulk(std::shared_ptr<ControlSession> self, ClipboardContent content,
    std::shared_ptr<std::atomic_bool> allowed, RequestId id, std::string reconfirm_transfer) {
    const auto sequence = content.sequence;
    const auto read_sequence = self->server_.clipboard_sequence;
    // Exclusion is checked before handing any file references to the disk worker.
    const bool excluded = content.excluded || std::ranges::any_of(content.omitted, [](const auto& omission) {
        return omission.reason == "excluded";
    });
    json::object response{{"type", reconfirm_transfer.empty() ? "clipboard" : "clipboard_bulk_reconfirm"},
                          {"sequence", sequence}, {"excluded", excluded}};
    if (!reconfirm_transfer.empty()) response["transfer_id"] = reconfirm_transfer;
    if (excluded) {
        if (!allowed->exchange(false)) co_return;
        self->snapshot_timer_->cancel();
        response["omitted"] = {json::object{{"format", "files"}, {"reason", "excluded"}}};
        self->reply(id, std::move(response));
        co_return;
    }
    const auto inspected = co_await bulk_disk<std::shared_ptr<ClipboardBulkSnapshot>>(self->server_,
        [content = std::move(content), allowed, read_sequence, sequence]() mutable {
            return ClipboardBulkSnapshot::inspect(std::move(content), [allowed, read_sequence, sequence] {
                return allowed->load() && read_sequence() == sequence;
            });
        });
    if (!allowed->exchange(false) || self->closing_) co_return;
    self->snapshot_timer_->cancel();
    if (!inspected || read_sequence() != sequence) {
        const auto code = inspected ? "source_changed" : inspected.error();
        if (reconfirm_transfer.empty()) self->send_error(id, code, "clipboard preflight failed");
        else self->send({{"type", "clipboard_bulk_failed"}, {"transfer_id", reconfirm_transfer}, {"code", code}});
        co_return;
    }
    self->bulk_snapshot_ = *inspected;
    self->bulk_snapshot_id_ = bulk::issue_socket_credentials().transfer_id_text;
    const auto& source = self->bulk_snapshot_->content();
    response["snapshot_id"] = self->bulk_snapshot_id_;
    response["manifest"] = self->bulk_snapshot_->preliminary();
    if (source.text) response["text"] = *source.text;
    if (source.html) response["html"] = *source.html;
    if (source.image) response["image"] = {{"mime_type", "image/png"}, {"size", source.image->png.size()},
        {"width", source.image->width}, {"height", source.image->height}};
    json::array omitted;
    for (const auto& omission : source.omitted) omitted.push_back({
        {"format", omission_name(omission.format)}, {"reason", omission.reason}});
    response["omitted"] = std::move(omitted);
    self->reply(id, std::move(response));
}

void ControlSession::bulk_request(const json::object& message, const RequestId& id) {
    if (!bulk_negotiated_) throw ProtocolError{"not_ready", "negotiate clipboard bulk first"};
    if (auth_generation_ != server_.auth_generation) {
        throw ProtocolError{"not_authenticated", "reconnect after authorization changed"};
    }
    if (bulk_) throw ProtocolError{"limit_reached", "one clipboard transfer is already active"};
    const auto snapshot = required_string(message, "snapshot_id");
    if (!bulk_snapshot_ || snapshot != bulk_snapshot_id_) {
        throw ProtocolError{"stale_snapshot", "obtain a current clipboard snapshot"};
    }
    auto entry = std::make_shared<BulkEntry>(bulk::issue_socket_credentials());
    entry->download = true;
    entry->created = entry->progress = server_.bulk_clock();
    entry->reported = entry->created;
    entry->source = bulk_snapshot_;
    entry->source_snapshot = bulk_snapshot_id_;
    entry->manifest = entry->source->manifest();
    bulk_ = entry;
    // Resolve the short request before copying. The final offer is a transfer-ID
    // event; no socket credential is exposed until the immutable spool is ready.
    reply(id, {{"type", "clipboard_bulk_progress"}, {"transfer_id", entry->credentials.transfer_id_text},
        {"source_snapshot_id", entry->source_snapshot}, {"phase", "preparing"}, {"completed_bytes", 0},
        {"total_bytes", bulk::payload_bytes(entry->manifest)}, {"completed_nodes", 0},
        {"total_nodes", entry->manifest.nodes.size()}});
    schedule_bulk_timeout();
    asio::co_spawn(ws_->get_executor(), prepare_download(shared_from_this(), entry), asio::detached);
}

awaitable<void> ControlSession::prepare_download(std::shared_ptr<ControlSession> self,
                                                std::shared_ptr<BulkEntry> entry) {
    const auto configured_root = self->server_.bulk_staging_root;
    const auto read_sequence = self->server_.clipboard_sequence;
    const auto clock = self->server_.bulk_clock;
    const auto observer = self->server_.bulk_prepare_observer;
    const auto write_check = self->server_.bulk_write_check;
    const auto result = co_await bulk_disk<bulk::Manifest>(self->server_,
        [entry, configured_root, read_sequence, clock, observer, write_check] {
            const auto root = configured_root.empty() ? clipboard_staging_root() : configured_root;
            entry->spool = std::make_unique<ClipboardBulkSpool>(root, entry->credentials.transfer_id_text,
                entry->source, [entry, read_sequence] {
                    return entry->allowed->load() && read_sequence() == entry->source->sequence();
                }, [entry, clock, observer](std::uint64_t bytes) {
                    entry->prepared_bytes.store(bytes);
                    entry->prepared_tick.store(clock().time_since_epoch().count());
                    if (observer) observer(bytes);
                }, "host", write_check);
            return entry->spool->manifest();
        });
    if (self->bulk_ != entry || entry->terminal) co_return;
    if (!result) {
        if (result.error() == "source_changed") {
            const auto transfer = entry->credentials.transfer_id_text;
            self->finish_bulk("source_changed", false, {}, false);
            self->get_bulk_clipboard(clipboard_all | clipboard_files, {}, transfer);
        } else self->finish_bulk(result.error());
        co_return;
    }
    if (read_sequence() != entry->source->sequence()) {
        const auto transfer = entry->credentials.transfer_id_text;
        self->finish_bulk("source_changed", false, {}, false);
        self->get_bulk_clipboard(clipboard_all | clipboard_files, {}, transfer);
        co_return;
    }
    entry->manifest = *result;
    entry->ready = true;
    entry->received = 0;
    entry->offered = entry->progress = self->server_.bulk_clock();
    auto preliminary = entry->source->preliminary();
    self->send({{"type", "clipboard_bulk_offer"}, {"direction", "host_to_client"},
        {"transfer_id", entry->credentials.transfer_id_text}, {"source_snapshot_id", entry->source_snapshot},
        {"source_sequence", entry->source->sequence()}, {"roots", std::move(preliminary.at("roots"))},
        {"manifest", bulk::manifest_json(entry->manifest)},
        {"url", "wss://" + self->authority_ + "/bulk/" + entry->credentials.transfer_id_text +
                    "?token=" + entry->credentials.token_text}, {"expires_in_ms", 10000}});
}

awaitable<void> ControlSession::send_bulk(std::shared_ptr<ControlSession> self, std::shared_ptr<BulkEntry> entry) {
    double tokens = bulk::max_queued_payload;
    auto replenished = Clock::now();
    try {
        entry->socket->binary(true);
        for (std::uint32_t node = 0; node < entry->manifest.nodes.size(); ++node) {
            const auto& declaration = entry->manifest.nodes[node];
            std::uint64_t offset{};
            while (offset < declaration.size) {
                const auto data = co_await bulk_disk<std::vector<std::byte>>(self->server_, [entry, node, offset] {
                    return entry->spool->read(node, offset);
                });
                if (entry->terminal) co_return;
                if (!data) { self->finish_bulk(data.error()); co_return; }
                SendActivity activity(self->server_, data->size());
                auto now = Clock::now();
                tokens = std::min<double>(bulk::max_queued_payload,
                    tokens + std::chrono::duration<double>(now - replenished).count() * 8 * 1024 * 1024);
                replenished = now;
                if (tokens < data->size()) {
                    asio::steady_timer throttle(self->ws_->get_executor());
                    throttle.expires_after(std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>((data->size() - tokens) / (8 * 1024 * 1024))));
                    co_await throttle.async_wait(use_awaitable);
                    if (entry->terminal) co_return;
                    now = Clock::now();
                    tokens = std::min<double>(bulk::max_queued_payload,
                        tokens + std::chrono::duration<double>(now - replenished).count() * 8 * 1024 * 1024);
                    replenished = now;
                }
                tokens -= data->size();
                const auto header = bulk::encode_header({
                    .kind = declaration.kind == bulk::Node::Kind::Png ? bulk::FrameKind::Png : bulk::FrameKind::File,
                    .transfer_id = entry->credentials.transfer_id, .node_index = node, .offset = offset,
                    .payload_size = static_cast<std::uint32_t>(data->size())});
                const std::array<asio::const_buffer, 2> buffers{asio::buffer(header), asio::buffer(*data)};
                entry->sending_ceiling = entry->received + data->size();
                activity.begin_write();
                co_await entry->socket->async_write(buffers, use_awaitable);
                activity.end_write();
                if (entry->terminal) co_return;
                offset += data->size();
                entry->received += data->size();
                entry->progress = self->server_.bulk_clock();
                if (entry->progress - entry->reported >= 250ms) {
                    entry->reported = entry->progress;
                    self->send({{"type", "clipboard_bulk_progress"}, {"transfer_id", entry->credentials.transfer_id_text},
                        {"phase", "sending"}, {"completed_bytes", entry->received},
                        {"total_bytes", bulk::payload_bytes(entry->manifest)}, {"completed_nodes", node},
                        {"total_nodes", entry->manifest.nodes.size()}});
                }
            }
        }
        const auto read_sequence = self->server_.clipboard_sequence;
        const auto current = co_await bulk_disk<bool>(self->server_, [entry, read_sequence] {
            return entry->source->unchanged([entry, read_sequence] {
                return entry->allowed->load() && read_sequence() == entry->source->sequence();
            });
        });
        if (entry->terminal) co_return;
        if (!current || !*current || read_sequence() != entry->source->sequence()) {
            self->finish_bulk("source_changed");
            co_return;
        }
        const auto end = bulk::encode_header({.kind = bulk::FrameKind::End,
            .transfer_id = entry->credentials.transfer_id, .node_index = UINT32_MAX,
            .offset = bulk::payload_bytes(entry->manifest), .payload_size = 0});
        entry->end_in_flight = true;
        co_await entry->socket->async_write(asio::buffer(end), use_awaitable);
        if (entry->terminal) co_return;
        entry->ended = true;
        entry->progress = self->server_.bulk_clock();
        if (entry->receiver_complete) self->complete_download();
    } catch (const boost::system::system_error&) {
        if (!entry->terminal) self->finish_bulk("interrupted");
    }
}

awaitable<void> ControlSession::watch_download_socket(std::shared_ptr<ControlSession> self,
                                                     std::shared_ptr<BulkEntry> entry) {
    beast::flat_buffer buffer(bulk::frame_header_size + bulk::max_chunk_size);
    try {
        co_await entry->socket->async_read(buffer, use_awaitable);
        if (!entry->terminal) self->finish_bulk("invalid_frame");
    } catch (const boost::system::system_error&) {
        if (!entry->terminal) self->finish_bulk("interrupted");
    }
}

void ControlSession::bulk_receiver_progress(const json::object& message, const RequestId&) {
    if (!bulk_ || !bulk_->download || !bulk_->socket ||
        required_string(message, "transfer_id") != bulk_->credentials.transfer_id_text) {
        throw ProtocolError{"invalid_request", "unknown outgoing clipboard transfer"};
    }
    const auto bytes = required_integer(message, "completed_bytes", 0, bulk::payload_bytes(bulk_->manifest));
    const auto nodes = required_integer(message, "completed_nodes", 0, bulk_->manifest.nodes.size());
    if (required_string(message, "phase") != "receiving" ||
        required_integer(message, "total_bytes", 0, bulk::payload_bytes(bulk_->manifest)) !=
            bulk::payload_bytes(bulk_->manifest) ||
        required_integer(message, "total_nodes", 0, bulk_->manifest.nodes.size()) != bulk_->manifest.nodes.size() ||
        bytes < bulk_->receiver_bytes || bytes > bulk_->sending_ceiling || nodes < bulk_->completed_nodes) {
        finish_bulk("invalid_progress");
        return;
    }
    // Replayed progress must not keep an otherwise stalled transfer alive.
    if (bytes != bulk_->receiver_bytes || nodes != bulk_->completed_nodes) bulk_->progress = server_.bulk_clock();
    bulk_->receiver_bytes = bytes;
    bulk_->completed_nodes = static_cast<std::uint32_t>(nodes);
}

void ControlSession::bulk_receiver_complete(const json::object& message, const RequestId&) {
    const auto transfer = required_string(message, "transfer_id");
    if (!bulk_ && transfer == last_bulk_id_) return; // identical receiver acknowledgement is idempotent.
    if (!bulk_ || !bulk_->download || transfer != bulk_->credentials.transfer_id_text) {
        throw ProtocolError{"invalid_request", "unknown outgoing clipboard transfer"};
    }
    if (!bulk_->end_in_flight) { finish_bulk("early_complete"); return; }
    bulk_->receiver_complete = true;
    if (bulk_->ended) complete_download();
}

void ControlSession::complete_download() {
    last_bulk_id_ = bulk_->credentials.transfer_id_text;
    last_bulk_source_ = bulk_->source_snapshot;
    finish_bulk({}, false, {}, false); // Only the receiver acknowledgement is success; do not echo it.
}

void ControlSession::bulk_receiver_failed(const json::object& message, const RequestId&) {
    if (!bulk_ || !bulk_->download ||
        required_string(message, "transfer_id") != bulk_->credentials.transfer_id_text) {
        throw ProtocolError{"invalid_request", "unknown outgoing clipboard transfer"};
    }
    // Do not echo a remote failure (or its arbitrary/private diagnostic text).
    finish_bulk("receiver_failed", false, {}, false);
}

} // namespace beamit::host::wss
