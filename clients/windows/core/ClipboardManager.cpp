#include "ClipboardManager.hpp"
#include "net/Protocol.hpp"

#include <boost/asio.hpp>
#include <algorithm>
#include <future>
#include <set>
#include <thread>

namespace beamit::client {
namespace asio = boost::asio;
namespace bulk = net::clipboard_bulk;
using namespace std::chrono_literals;
using namespace host;
using Clock = std::chrono::steady_clock;

namespace {
struct InvalidReply : std::runtime_error { InvalidReply() : std::runtime_error("invalid clipboard reply") {} };
void require(bool condition) { if (!condition) throw InvalidReply(); }
std::string text(const json::object& object, std::string_view key) {
    const auto value = string_field(object, key);
    require(value.has_value());
    return std::string(*value);
}
std::uint64_t number(const json::object& object, std::string_view key) {
    const auto* value = object.if_contains(key);
    require(value && (value->is_uint64() || (value->is_int64() && value->as_int64() >= 0)));
    return value->to_number<std::uint64_t>();
}
std::uint64_t file_bytes(const bulk::Manifest& manifest) {
    std::uint64_t total{};
    for (const auto& node : manifest.nodes) if (node.kind == bulk::Node::Kind::File) total += node.size;
    return total;
}
bulk::Manifest manifest(const json::object& object, bool preliminary = false) {
    const auto* value = object.if_contains("manifest");
    require(value && value->is_object());
    auto copy = value->as_object();
    if (preliminary) {
        require(copy.contains("nodes") && copy.at("nodes").is_array());
        for (auto& node : copy.at("nodes").as_array()) {
            require(node.is_object());
            if (string_field(node.as_object(), "type") != "directory") {
                require(!node.as_object().contains("sha256"));
                node.as_object()["sha256"] = net::base64url_encode(net::Sha256{});
            }
        }
    }
    const auto parsed = bulk::parse_manifest(copy, json::serialize(copy).size());
    require(parsed.has_value());
    require(std::ranges::count_if(parsed->nodes, [](const auto& node) {
        return node.kind == bulk::Node::Kind::Png;
    }) <= 1);
    return *parsed;
}
ClipboardWrite metadata(const json::object& reply) {
    ClipboardWrite write;
    if (reply.contains("text")) write.text = text(reply, "text");
    if (reply.contains("html")) write.html = text(reply, "html");
    require(write.text.value_or("").size() + write.html.value_or("").size() <= net::protocol::max_clipboard_bytes);
    return write;
}
} // namespace

struct ClipboardManager::State : std::enable_shared_from_this<State> {
    struct Operation {
        Context context;
        std::uint64_t revision{};
        std::shared_ptr<std::atomic_bool> allowed = std::make_shared<std::atomic_bool>(true);
        bool upload{}, manual{}, ended{}, commit_sent{}, publishing{}, receiving{};
        DWORD sequence{};
        std::uint64_t remote_sequence{};
        std::string id, source_id, local_id, url;
        bulk::Manifest manifest;
        json::value roots;
        ClipboardWrite metadata;
        std::shared_ptr<ClipboardBulkSnapshot> source;
        std::unique_ptr<ClipboardBulkSpool> spool; // disk worker only
        std::unique_ptr<ClipboardBulkReceiver> receiver; // disk worker, then native callback ownership
        std::atomic<std::uint64_t> prepared{};
        std::uint64_t bytes{}, offset{};
        std::uint32_t node{};
        Clock::time_point created{Clock::now()}, progress{created}, reported{}, replenished{created};
        double tokens{bulk::max_queued_payload};
        std::function<void(bool)> paste;
    };
    Session& session;
    Session::Dispatcher dispatch;
    Callbacks callbacks;
    Native native;
    std::unique_ptr<ClipboardMonitor> monitor;
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> keep{io.get_executor()};
    asio::thread_pool disk{1};
    asio::steady_timer timer{io}, debounce{io}, throttle{io};
    std::thread thread;
    std::atomic_bool alive{true};
    std::atomic<std::shared_ptr<std::atomic_bool>> publication;
    std::atomic<std::uint64_t> revision{};
    std::mutex context_mutex;
    Context submitted;
    Context context;
    bulk::TransferPolicy policy;
    enum class Mode { Negotiating, Bulk, Legacy, Failed } mode{Mode::Negotiating};
    std::shared_ptr<Operation> current;
    std::optional<DWORD> uploaded;

    State(Session& session_, Session::Dispatcher dispatch_, Callbacks callbacks_, Native native_)
        : session(session_), dispatch(std::move(dispatch_)), callbacks(std::move(callbacks_)), native(std::move(native_)) {}

    template<class F> void post(F function) {
        asio::post(io, [weak = weak_from_this(), function = std::move(function)]() mutable {
            if (auto self = weak.lock(); self && self->alive.load()) {
                try { function(*self); }
                catch (const InvalidReply&) { self->finish(false, "invalid_reply"); }
            }
        });
    }
    template<class T, class F, class Done> void work(F function, Done done) {
        asio::post(disk, [weak = weak_from_this(), function = std::move(function), done = std::move(done)]() mutable {
            std::expected<T, SessionError> result = std::unexpected(SessionError{"storage_failed", "Clipboard storage failed."});
            try { result = function(); }
            catch (const ClipboardBulkStorageError& error) {
                result = std::unexpected(SessionError{error.code(), "Clipboard storage or source validation failed."});
            }
            catch (const std::runtime_error&) {
                // Storage/Win32/CNG errors contain private paths or API details.
                // Translate at this boundary, but do not hide programming/allocation errors.
                result = std::unexpected(SessionError{"storage_failed", "Clipboard storage failed."});
            }
            if (auto self = weak.lock()) self->post([done = std::move(done), result = std::move(result)](State& state) mutable {
                done(state, std::move(result));
            });
        });
    }
    bool valid(const std::shared_ptr<Operation>& op, bool check_revision = true) const {
        return current == op && op->allowed->load() && context == op->context &&
            context.active && (context.enabled || op->manual) &&
            (!context.foreground || GetForegroundWindow() == context.foreground) &&
            (!check_revision || native.sequence() == op->sequence);
    }
    void show(std::string phase, bool force = false, std::string error = {}) {
        const auto op = current;
        if (op && !force && Clock::now() - op->reported < 250ms) return;
        if (op) op->reported = Clock::now();
        Status status{std::move(phase), op ? op->bytes : 0, op ? bulk::payload_bytes(op->manifest) : 0,
                      bool(op), std::move(error)};
        if (callbacks.status) dispatch([weak = weak_from_this(), status = std::move(status)]() mutable {
            if (auto self = weak.lock(); self && self->alive.load()) self->callbacks.status(std::move(status));
        });
    }
    void finish(bool success, std::string code = {}, bool notify = true) {
        const auto op = std::exchange(current, {});
        if (op) {
            op->allowed->store(false);
            timer.cancel(); throttle.cancel();
            session.close_bulk();
            if (notify && !op->id.empty()) {
                const bool cancellation = op->upload || code == "cancelled" || code == "disconnected" ||
                    code == "superseded" || code == "focus_or_enablement_changed" || code == "declined_or_stale";
                session.send({{"type", cancellation ? "clipboard_bulk_cancel" : "clipboard_bulk_failed"},
                    {"transfer_id", op->id}, {"code", code}});
            }
            asio::post(disk, [op] {
                op->spool.reset();
                if (!op->publishing) op->receiver.reset();
            });
            if (op->paste) dispatch([weak = weak_from_this(), op, done = std::move(op->paste), success] {
                const auto self = weak.lock();
                done(success && self && self->alive.load() && self->revision.load() == op->revision &&
                    self->native.sequence() == op->sequence &&
                    (!op->context.foreground || GetForegroundWindow() == op->context.foreground));
            });
        }
        show(success ? "complete" : "cancelled", true, std::move(code));
    }
    std::shared_ptr<Operation> begin(bool upload, bool manual) {
        if (!context.active || (!context.enabled && !manual)) return {};
        if (mode == Mode::Negotiating || mode == Mode::Failed) {
            if (manual) show("failed", true, mode == Mode::Negotiating ? "not_ready" : "negotiation_failed");
            return {};
        }
        if (current) finish(false, "superseded");
        auto op = current = std::make_shared<Operation>();
        op->context = context; op->upload = upload; op->manual = manual;
        op->revision = revision.load();
        op->sequence = native.sequence();
        op->created = op->progress = native.clock();
        op->local_id = bulk::issue_socket_credentials().transfer_id_text;
        op->source_id = op->local_id;
        publication.store(op->allowed);
        show("preparing", true);
        tick(op);
        return op;
    }
    void tick(std::shared_ptr<Operation> op) {
        timer.expires_after(250ms);
        timer.async_wait([weak = weak_from_this(), op](boost::system::error_code error) {
            auto self = weak.lock();
            if (error || !self || self->current != op) return;
            if (!self->valid(op, !op->publishing)) { self->finish(false, "stale_clipboard_or_focus"); return; }
            if (op->prepared.load() > op->bytes) {
                op->bytes = op->prepared.load();
                op->progress = self->native.clock();
                self->show("preparing");
            }
            const auto now = self->native.clock();
            if (now - op->created >= 30min || now - op->progress >= 30s) {
                self->finish(false, "timeout");
                return;
            }
            self->tick(op);
        });
    }
    template<class F> void request(std::string type, json::object fields, std::string reply, F done) {
        session.request(std::move(type), std::move(fields), std::move(reply),
            [weak = weak_from_this(), done = std::move(done)](auto result) mutable {
                if (auto self = weak.lock()) self->post([done = std::move(done), result = std::move(result)](State& state) mutable {
                    done(state, std::move(result));
                });
            });
    }
    void start() {
        request("clipboard_bulk_negotiate", {{"version", 1}}, "clipboard_bulk_capabilities",
            [](State& self, auto result) {
                if (!result) {
                    if (result.error().code == "unknown_type") { self.mode = Mode::Legacy; self.show("ready", true); }
                    else { self.mode = Mode::Failed; self.show("failed", true, result.error().code); }
                    return;
                }
                try {
                    require(number(*result, "version") == 1 && number(*result, "max_file_bytes") == bulk::max_file_bytes &&
                        number(*result, "max_roots") == bulk::max_roots && number(*result, "max_nodes") == bulk::max_nodes &&
                        number(*result, "max_chunk_bytes") == bulk::max_chunk_size &&
                        number(*result, "max_manifest_bytes") == bulk::max_manifest_bytes &&
                        number(*result, "max_png_bytes") == bulk::max_png_bytes &&
                        number(*result, "max_png_dimension") == bulk::max_png_dimension &&
                        number(*result, "max_png_pixels") == bulk::max_png_pixels &&
                        number(*result, "max_component_bytes") == 255 && number(*result, "max_path_bytes") == 4096 &&
                        number(*result, "staging_quota_bytes") == 8'000'000'000ULL &&
                        number(*result, "scratch_bytes") == 128 * 1024 * 1024);
                    self.mode = Mode::Bulk;
                    self.show("ready", true);
                } catch (const InvalidReply&) { self.mode = Mode::Failed; self.show("failed", true, "invalid_capabilities"); }
            });
    }
    template<class Next> void confirm(std::shared_ptr<Operation> op, Next next) {
        if (!valid(op)) { finish(false, "stale_clipboard_or_focus"); return; }
        if (!bulk::requires_confirmation(file_bytes(op->manifest), op->manifest.selected_roots, policy)) {
            next(*this, op); return;
        }
        show("waiting for confirmation", true);
        if (!callbacks.confirm) { finish(false, "confirmation_unavailable"); return; }
        dispatch([weak = weak_from_this(), op, next = std::move(next)]() mutable {
            if (auto self = weak.lock(); self && self->alive.load() && op->allowed->load()) self->callbacks.confirm(
                op->manifest.selected_roots, file_bytes(op->manifest),
                [weak, op, next = std::move(next)](bool approved) mutable {
                    if (auto self = weak.lock()) self->post([op, approved, next = std::move(next)](State& state) mutable {
                        if (state.current != op) return;
                        if (!approved || !state.valid(op)) { state.finish(false, "declined_or_stale"); return; }
                        op->progress = state.native.clock();
                        next(state, op);
                    });
                });
        });
    }
    void upload(bool manual, std::function<void(bool)> paste = {}) {
        if (current && current->upload && current->sequence == native.sequence() && valid(current)) {
            if (paste) current->paste = std::move(paste);
            return;
        }
        auto op = begin(true, manual);
        if (!op) { if (paste) dispatch([paste = std::move(paste)] { paste(false); }); return; }
        op->paste = std::move(paste);
        native.read(clipboard_all | (mode == Mode::Bulk ? clipboard_files : 0U), mode == Mode::Bulk,
            [weak = weak_from_this(), op](ClipboardReadResult result) mutable {
                if (auto self = weak.lock()) self->post([op, result = std::move(result)](State& state) mutable {
                    if (state.current != op) return;
                    if (!result.content || !state.valid(op) || result.content->sequence != op->sequence) {
                        state.finish(false, "stale_or_unavailable"); return;
                    }
                    auto content = std::move(*result.content);
                    if (content.excluded || std::ranges::any_of(content.omitted, [](const auto& item) {
                        return item.reason == "excluded" || item.format == clipboard_files;
                    })) { state.finish(false, "excluded_or_unsupported"); return; }
                    op->metadata.text = content.text; op->metadata.html = content.html;
                    if (state.mode == Mode::Legacy || (content.files.empty() && !content.image)) {
                        json::object fields;
                        if (content.text) fields["text"] = *content.text;
                        if (content.html) fields["html"] = *content.html;
                        if (content.image) fields["image"] = {{"mime_type", "image/png"},
                            {"data", net::base64_encode(content.image->png)}};
                        if (fields.empty()) { state.finish(false, "unsupported_clipboard"); return; }
                        state.request("set_clipboard", std::move(fields), "clipboard_set", [op](State& self, auto reply) {
                            if (self.current != op) return;
                            if (!reply || !self.valid(op)) { self.finish(false, reply ? "stale_clipboard" : reply.error().code); return; }
                            self.uploaded = op->sequence;
                            self.finish(true, {}, false);
                        });
                        return;
                    }
                    const auto sequence = state.native.sequence;
                    state.work<std::shared_ptr<ClipboardBulkSnapshot>>(
                        [op, sequence, content = std::move(content)]() mutable {
                            return ClipboardBulkSnapshot::inspect(std::move(content), [op, sequence] {
                                return op->allowed->load() && sequence() == op->sequence;
                            });
                        }, [op](State& self, auto source) {
                            if (self.current != op) return;
                            if (!source || !self.valid(op)) { self.finish(false, source ? "stale_clipboard" : source.error().code); return; }
                            op->source = *source; op->manifest = op->source->manifest();
                            self.confirm(op, [](State& state, auto operation) { state.spool(operation); });
                        });
                });
            });
    }
    void spool(std::shared_ptr<Operation> op) {
        show("preparing", true);
        const auto sequence = native.sequence;
        const auto root = native.staging_root;
        work<bulk::Manifest>([op, sequence, root] {
            op->spool = std::make_unique<ClipboardBulkSpool>(root, op->local_id, op->source,
                [op, sequence] {
                    return op->allowed->load() && sequence() == op->sequence &&
                        (!op->context.foreground || GetForegroundWindow() == op->context.foreground);
                }, [op](std::uint64_t bytes) { op->prepared.store(bytes); }, "client");
            return op->spool->manifest();
        }, [op](State& self, auto result) {
            if (self.current != op) return;
            if (!result || !self.valid(op)) {
                const bool changed = (!result && result.error().code == "source_changed");
                const bool manual = op->manual;
                self.finish(false, changed ? "source_changed_reconfirm" :
                    result ? "stale_clipboard" : result.error().code);
                // A fresh snapshot and decision; never carry approval to a changed selection.
                if (changed) self.upload(manual);
                return;
            }
            op->manifest = *result; op->bytes = 0; op->prepared.store(0);
            json::object fields{{"source_snapshot_id", op->source_id}, {"manifest", bulk::manifest_json(op->manifest)}};
            if (op->metadata.text) fields["text"] = *op->metadata.text;
            if (op->metadata.html) fields["html"] = *op->metadata.html;
            self.request("clipboard_bulk_begin", std::move(fields), "clipboard_bulk_offer",
                [op](State& state, auto offer) {
                    if (state.current != op) return;
                    if (!offer) { state.finish(false, offer.error().code); return; }
                    state.offer(op, *offer);
                });
        });
    }
    void download(bool manual) {
        auto op = begin(false, manual);
        if (!op) return;
        request("get_clipboard", {}, "clipboard", [op](State& self, auto reply) {
            if (self.current != op) return;
            if (!reply || !self.valid(op)) { self.finish(false, reply ? "stale_clipboard" : reply.error().code); return; }
            self.snapshot(op, *reply);
        });
    }
    void snapshot(std::shared_ptr<Operation> op, const json::object& reply) {
        require(valid(op));
        if (const auto* excluded = reply.if_contains("excluded"); excluded && *excluded == true) {
            finish(false, "excluded"); return;
        }
        if (const auto* omitted = reply.if_contains("omitted"); omitted && omitted->is_array()) {
            for (const auto& entry : omitted->as_array()) if (entry.is_object() &&
                string_field(entry.as_object(), "reason") == "excluded") { finish(false, "excluded"); return; }
        }
        op->metadata = metadata(reply);
        if (mode == Mode::Legacy) {
            if (const auto* image = reply.if_contains("image")) {
                require(image->is_object());
                require(text(image->as_object(), "mime_type") == "image/png");
                op->metadata.image = text(image->as_object(), "data");
            }
            if (!op->metadata.text && !op->metadata.html && !op->metadata.image) { finish(false, "unsupported_clipboard"); return; }
            publish(op, op->metadata);
            return;
        }
        op->source_id = text(reply, "snapshot_id");
        op->remote_sequence = number(reply, "sequence");
        require(op->remote_sequence <= UINT32_MAX);
        require(bulk::valid_transfer_id(op->source_id));
        op->manifest = manifest(reply, true);
        const auto& object = reply.at("manifest").as_object();
        require(object.contains("roots") && object.at("roots").is_array() &&
            object.at("roots").as_array().size() == op->manifest.selected_roots);
        op->roots = object.at("roots");
        std::set<std::string> paths;
        for (const auto& root : op->roots.as_array()) {
            require(root.is_object());
            const auto path = text(root.as_object(), "path");
            const auto identity = net::base64url_decode(text(root.as_object(), "identity"));
            require(paths.insert(path).second && identity && identity->size() == 32 &&
                std::ranges::any_of(op->manifest.nodes, [&](const auto& node) {
                return node.kind != bulk::Node::Kind::Png && node.path == path && path.find('/') == std::string::npos;
            }));
        }
        if (op->manifest.nodes.empty()) { publish(op, op->metadata); return; }
        confirm(op, [](State& self, auto operation) {
            self.request("clipboard_bulk_request", {{"snapshot_id", operation->source_id}}, "clipboard_bulk_progress",
                [operation](State& state, auto reply) {
                    if (state.current != operation) return;
                    if (!reply || !state.valid(operation)) {
                        state.finish(false, reply ? "stale_clipboard" : reply.error().code); return;
                    }
                    require(text(*reply, "phase") == "preparing" &&
                        text(*reply, "source_snapshot_id") == operation->source_id);
                    operation->id = text(*reply, "transfer_id");
                    require(bulk::valid_transfer_id(operation->id));
                    operation->progress = state.native.clock();
                    state.show("preparing", true);
                });
        });
    }
    void offer(std::shared_ptr<Operation> op, const json::object& reply) {
        if (!valid(op)) { finish(false, "stale_clipboard_or_focus"); return; }
        const auto final = manifest(reply);
        require(text(reply, "direction") == (op->upload ? "client_to_host" : "host_to_client") &&
            text(reply, "source_snapshot_id") == op->source_id && number(reply, "expires_in_ms") == 10000);
        require(op->upload ? number(reply, "destination_sequence") <= UINT32_MAX :
            number(reply, "source_sequence") == op->remote_sequence);
        auto expected = op->manifest;
        auto received = final;
        if (!op->upload) {
            for (auto& node : received.nodes) node.sha256 = {};
            require(reply.contains("roots") && reply.at("roots") == op->roots);
        }
        require(bulk::manifest_json(expected) == bulk::manifest_json(received));
        const auto id = text(reply, "transfer_id");
        require(bulk::valid_transfer_id(id) && (op->id.empty() || op->id == id));
        op->id = id; op->manifest = final;
        if (!op->upload) { op->bytes = 0; op->prepared.store(0); op->receiving = true; }
        const auto url = text(reply, "url");
        require(url.starts_with("wss://"));
        const auto slash = url.find('/', 6);
        const auto endpoint = session.endpoint();
        require(endpoint && slash != std::string::npos && endpoint->same_origin(std::string_view(url).substr(6, slash - 6)));
        op->url = url.substr(slash);
        const auto prefix = "/bulk/" + op->id + "?token=";
        require(op->url.starts_with(prefix));
        const auto token = net::base64url_decode(std::string_view(op->url).substr(prefix.size()));
        require(token && token->size() == 32 && op->url.find_first_of("&#") == std::string::npos);
        if (op->upload) open(op);
        else {
            const auto root = native.staging_root;
            work<bool>([root, op] {
                if (!op->allowed->load()) throw ClipboardBulkStorageError("cancelled");
                op->receiver = std::make_unique<ClipboardBulkReceiver>(root, op->id, op->manifest, "client");
                return true;
            }, [op](State& self, auto result) {
                if (self.current != op) return;
                if (!result || !self.valid(op)) { self.finish(false, result ? "stale_clipboard" : result.error().code); return; }
                self.open(op);
            });
        }
    }
    void open(std::shared_ptr<Operation> op) {
        if (!valid(op)) { finish(false, "stale_clipboard_or_focus"); return; }
        if (!op->upload) { connect_bulk(op); return; }
        const auto sequence = native.sequence;
        work<bool>([op, sequence] {
            return op->source->unchanged([op, sequence] { return op->allowed->load() && sequence() == op->sequence; });
        }, [op](State& self, auto result) {
            if (self.current != op) return;
            if (!result || !*result || !self.valid(op)) { self.finish(false, "source_changed"); return; }
            self.connect_bulk(op);
        });
    }
    void connect_bulk(std::shared_ptr<Operation> op) {
        if (!valid(op)) { finish(false, "stale_clipboard_or_focus"); return; }
        BulkCallbacks events;
        events.failed = [weak = weak_from_this(), op](const SessionError& error) {
            if (auto self = weak.lock()) self->post([op, code = error.code](State& state) {
                // Host publication may close /bulk before its control acknowledgement
                // arrives. END is not success; retain the control-outcome deadline.
                if (op->upload && op->ended &&
                    (code == "disconnected" || code == "network_error" || code == "tls_failed")) return;
                if (state.current == op) state.finish(false, code);
            });
        };
        events.consume = [weak = weak_from_this(), op](std::vector<std::byte> bytes, auto done) mutable {
            if (auto self = weak.lock(); self && self->alive.load()) {
                self->post([op, bytes = std::move(bytes), done = std::move(done)](State& state) mutable {
                    state.receive(op, std::move(bytes), std::move(done));
                });
            } else done(std::unexpected(SessionError{"cancelled", "Clipboard transfer ended."}));
        };
        session.open_bulk(op->url, 10s, std::move(events),
            [weak = weak_from_this(), op](auto result) mutable {
                if (auto self = weak.lock()) self->post([op, result = std::move(result)](State& state) {
                    if (state.current != op) return;
                    if (!result || !state.valid(op)) { state.finish(false, result ? "stale_clipboard" : result.error().code); return; }
                    op->progress = state.native.clock();
                    state.show(op->upload ? "sending" : "receiving", true);
                    if (op->upload) state.send_next(op);
                });
            });
    }
    void receive(std::shared_ptr<Operation> op, std::vector<std::byte> bytes,
                 std::function<void(std::expected<void, SessionError>)> done) {
        if (!valid(op) || op->upload || op->ended) {
            done(std::unexpected(SessionError{"invalid_frame", "Unexpected clipboard frame."}));
            if (current == op) finish(false, "invalid_frame");
            return;
        }
        const auto decoded = bulk::decode_header(bytes);
        if (!decoded) { done(std::unexpected(SessionError{"invalid_frame", "Invalid clipboard frame."})); finish(false, "invalid_frame"); return; }
        replenish(op);
        if (op->tokens < decoded->payload_size) {
            throttle.expires_after(std::chrono::ceil<Clock::duration>(
                std::chrono::duration<double>((decoded->payload_size - op->tokens) / (8 * 1024 * 1024))));
            throttle.async_wait([weak = weak_from_this(), op, bytes = std::move(bytes), done = std::move(done)](
                boost::system::error_code error) mutable {
                if (auto self = weak.lock(); self && !error) self->receive(op, std::move(bytes), std::move(done));
                else done(std::unexpected(SessionError{"cancelled", "Clipboard receipt ended."}));
            });
            return; // Hold the transport drain completion; never queue another frame.
        }
        op->tokens -= decoded->payload_size;
        work<bool>([op, bytes = std::move(bytes)] {
            const auto result = op->receiver->accept(bytes);
            if (!result) throw ClipboardBulkStorageError(result.error());
            return true;
        }, [op, header = *decoded, done = std::move(done)](State& self, auto result) mutable {
            if (self.current != op || !self.valid(op) || !result) {
                done(std::unexpected(SessionError{result ? "cancelled" : result.error().code, "Clipboard receipt failed."}));
                if (self.current == op) self.finish(false, result ? "stale_clipboard" : result.error().code);
                return;
            }
            op->bytes += header.payload_size;
            op->progress = self.native.clock();
            if (header.kind == bulk::FrameKind::End) {
                op->ended = true; op->node = static_cast<std::uint32_t>(op->manifest.nodes.size());
            } else if (header.offset + header.payload_size == op->manifest.nodes[header.node_index].size) {
                op->node = header.node_index + 1;
            }
            if (Clock::now() - op->reported >= 250ms) {
                self.session.send({{"type", "clipboard_bulk_progress"}, {"transfer_id", op->id},
                    {"phase", "receiving"}, {"completed_bytes", op->bytes}, {"total_bytes", bulk::payload_bytes(op->manifest)},
                    {"completed_nodes", op->node}, {"total_nodes", op->manifest.nodes.size()}});
                self.show("receiving");
            }
            done({});
            if (!op->ended) return;
            self.work<ClipboardWrite>([op] {
                auto write = op->metadata;
                const auto roots = op->receiver->promote();
                if (!roots) throw ClipboardBulkStorageError(roots.error());
                write.files = *roots;
                const auto png = op->receiver->prepared_png();
                if (!png) throw ClipboardBulkStorageError(png.error());
                if (!png->empty()) write.png = *png;
                return write;
            }, [op](State& state, auto write) {
                if (state.current != op) return;
                if (!write || !state.valid(op)) { state.finish(false, write ? "stale_clipboard" : write.error().code); return; }
                state.publish(op, std::move(*write));
            });
        });
    }
    void publish(std::shared_ptr<Operation> op, ClipboardWrite write) {
        if (!valid(op)) { finish(false, "stale_clipboard_or_focus"); return; }
        write.expected_sequence = op->sequence;
        write.publication_allowed = op->allowed;
        if (op->context.foreground) write.expected_foreground = op->context.foreground;
        op->publishing = true;
        show("publishing", true);
        native.write(std::move(write), [weak = weak_from_this(), op](ClipboardWriteResult result) {
            if (op->receiver && (result.error_code.empty() || result.retain_files)) op->receiver->published();
            if (auto self = weak.lock()) {
                asio::post(self->disk, [op] { op->receiver.reset(); });
                self->post([op, result = std::move(result)](State& state) {
                    if (state.current != op) return;
                    if (!result.error_code.empty() || !state.valid(op, false)) {
                        state.finish(false, result.error_code.empty() ? "stale_focus" : result.error_code);
                        return;
                    }
                    state.uploaded = result.sequence; // Never echo our own replacement.
                    if (!op->id.empty()) state.session.send({{"type", "clipboard_bulk_complete"},
                        {"transfer_id", op->id}, {"sequence", result.sequence}});
                    state.finish(true, {}, false);
                });
            }
        });
    }
    void send_next(std::shared_ptr<Operation> op) {
        if (!valid(op)) { finish(false, "stale_clipboard_or_focus"); return; }
        while (op->node < op->manifest.nodes.size() && op->offset == op->manifest.nodes[op->node].size) {
            ++op->node; op->offset = 0;
        }
        if (op->node == op->manifest.nodes.size()) {
            const auto sequence = native.sequence;
            work<bool>([op, sequence] {
                return op->source->unchanged([op, sequence] { return op->allowed->load() && sequence() == op->sequence; });
            }, [op](State& self, auto result) {
                if (self.current != op) return;
                if (!result || !*result || !self.valid(op)) { self.finish(false, "source_changed"); return; }
                self.write(op, {}, true);
            });
            return;
        }
        work<std::vector<std::byte>>([op] { return op->spool->read(op->node, op->offset); },
            [op](State& self, auto result) {
                if (self.current != op) return;
                if (!result || !self.valid(op)) { self.finish(false, result ? "stale_clipboard" : result.error().code); return; }
                replenish(op);
                if (op->tokens < result->size()) {
                    self.throttle.expires_after(std::chrono::ceil<Clock::duration>(
                        std::chrono::duration<double>((result->size() - op->tokens) / (8 * 1024 * 1024))));
                    self.throttle.async_wait([weak = self.weak_from_this(), op, bytes = std::move(*result)](
                        boost::system::error_code error) mutable {
                        if (auto state = weak.lock(); state && !error && state->current == op) state->write(op, std::move(bytes), false);
                    });
                } else self.write(op, std::move(*result), false);
            });
    }
    static void replenish(const std::shared_ptr<Operation>& op) {
        const auto now = Clock::now();
        op->tokens = std::min<double>(bulk::max_queued_payload,
            op->tokens + std::chrono::duration<double>(now - op->replenished).count() * 8 * 1024 * 1024);
        op->replenished = now;
    }
    void write(std::shared_ptr<Operation> op, std::vector<std::byte> bytes, bool end) {
        if (!valid(op)) { finish(false, "stale_clipboard_or_focus"); return; }
        const auto raw = net::base64url_decode(op->id);
        require(raw && raw->size() == 16);
        std::array<std::byte, 16> transfer{};
        std::ranges::copy(*raw, transfer.begin());
        const auto payload = bytes.size();
        const auto header = bulk::encode_header({
            .kind = end ? bulk::FrameKind::End : op->manifest.nodes[op->node].kind == bulk::Node::Kind::Png ?
                bulk::FrameKind::Png : bulk::FrameKind::File,
            .transfer_id = transfer, .node_index = end ? UINT32_MAX : op->node,
            .offset = end ? bulk::payload_bytes(op->manifest) : op->offset,
            .payload_size = static_cast<std::uint32_t>(payload)});
        bytes.insert(bytes.begin(), header.begin(), header.end());
        // Account for elapsed throttle time before spending it. Otherwise the next
        // chunk would receive the same elapsed credit again and double the rate.
        replenish(op);
        op->tokens = std::max(0.0, op->tokens - payload);
        session.write_bulk(std::move(bytes), [weak = weak_from_this(), op, payload, end](auto result) mutable {
            if (auto self = weak.lock()) self->post([op, payload, end, result = std::move(result)](State& state) {
                if (state.current != op) return;
                if (!result || !state.valid(op)) { state.finish(false, result ? "stale_clipboard" : result.error().code); return; }
                op->progress = state.native.clock();
                op->offset += payload; op->bytes += payload;
                state.show("sending");
                if (!end) { state.send_next(op); return; }
                op->ended = true;
                const auto sequence = state.native.sequence;
                state.work<bool>([op, sequence] {
                    return op->source->unchanged([op, sequence] {
                        return op->allowed->load() && sequence() == op->sequence;
                    });
                }, [op](State& self, auto unchanged) {
                    if (self.current != op) return;
                    if (!unchanged || !*unchanged || !self.valid(op)) { self.finish(false, "source_changed"); return; }
                    op->commit_sent = true;
                    self.request("clipboard_bulk_commit", {{"transfer_id", op->id}, {"source_snapshot_id", op->source_id}},
                        "clipboard_bulk_committed", [op](State& state, auto committed) {
                            if (state.current != op) return;
                            if (!committed) { state.finish(false, committed.error().code); return; }
                            require(text(*committed, "transfer_id") == op->id);
                        });
                });
            });
        });
    }
    void event(const json::object& message) {
        const auto type = string_field(message, "type").value_or("");
        if (type == "clipboard_changed") {
            uploaded.reset();
            if (const auto* excluded = message.if_contains("excluded"); excluded && *excluded == true) {
                if (current) finish(false, "excluded");
                return;
            }
            if (context.enabled && context.active) download(false);
            return;
        }
        const auto op = current;
        if (!op || string_field(message, "transfer_id") != op->id) return;
        if (type == "clipboard_bulk_offer" && !op->upload) { offer(op, message); return; }
        if (type == "clipboard_bulk_reconfirm" && !op->upload) {
            op->id.clear(); op->bytes = 0; op->progress = native.clock();
            snapshot(op, message); return;
        }
        if (type == "clipboard_bulk_complete" && op->upload) {
            if (!op->ended || !op->commit_sent || !valid(op)) { finish(false, "premature_or_stale_completion"); return; }
            require(number(message, "sequence") <= UINT32_MAX);
            const auto sequence = native.sequence;
            work<bool>([op, sequence] {
                return op->source->unchanged([op, sequence] {
                    return op->allowed->load() && sequence() == op->sequence;
                });
            }, [op](State& self, auto unchanged) {
                if (self.current != op) return;
                if (!unchanged || !*unchanged || !self.valid(op)) { self.finish(false, "source_changed"); return; }
                self.uploaded = op->sequence;
                self.finish(true, {}, false);
            });
        } else if (type == "clipboard_bulk_failed" || type == "clipboard_bulk_cancelled") {
            finish(false, std::string(string_field(message, "code").value_or("remote_failure")), false);
        } else if (type == "clipboard_bulk_progress") {
            const auto bytes = number(message, "completed_bytes");
            require(number(message, "total_bytes") == bulk::payload_bytes(op->manifest) &&
                number(message, "total_nodes") == op->manifest.nodes.size() &&
                number(message, "completed_nodes") <= op->manifest.nodes.size() &&
                bytes <= bulk::payload_bytes(op->manifest));
            if (!op->upload && string_field(message, "phase") == "preparing" && !op->receiving &&
                bytes > op->bytes) { op->bytes = bytes; op->progress = native.clock(); show("preparing"); }
        }
    }
};

ClipboardManager::ClipboardManager(Session& session, Session::Dispatcher dispatcher, Callbacks callbacks, Native native)
    : state_(std::make_shared<State>(session, std::move(dispatcher), std::move(callbacks), std::move(native))) {
    auto& state = *state_;
    if (state.native.staging_root.empty()) state.native.staging_root = clipboard_staging_root();
    if (!state.native.read || !state.native.write) {
        state.monitor = std::make_unique<ClipboardMonitor>([weak = std::weak_ptr<State>(state_)](const ClipboardChange& change) {
            if (auto self = weak.lock()) self->post([change](State& state) {
                if (change.excluded) { if (state.current && state.current->upload) state.finish(false, "excluded"); return; }
                if (state.current && state.current->upload && change.sequence != state.current->sequence) {
                    state.finish(false, "source_changed");
                }
                if (!state.context.enabled || !state.context.active || change.sequence == state.uploaded) return;
                state.debounce.expires_after(80ms);
                state.debounce.async_wait([weak = state.weak_from_this()](boost::system::error_code error) {
                    if (auto self = weak.lock(); self && !error) self->upload(false);
                });
            });
        });
        state.native.read = [monitor = state.monitor.get()](unsigned formats, bool binary, auto done) {
            if (binary) monitor->read_bulk(formats, std::move(done));
            else monitor->read(formats, net::protocol::max_clipboard_bytes, std::move(done));
        };
        state.native.write = [monitor = state.monitor.get()](ClipboardWrite write, auto done) {
            monitor->write(std::move(write), std::move(done));
        };
    }
    state.work<bool>([root = state.native.staging_root] { ClipboardBulkReceiver::recover(root); return true; },
        [](State& self, auto result) { if (!result) self.show("failed", true, result.error().code); });
    state.thread = std::thread([raw = state_.get()] { raw->io.run(); });
}
ClipboardManager::~ClipboardManager() {
    auto state = state_;
    if (auto guard = state->publication.load()) guard->store(false);
    std::promise<void> stopped;
    asio::post(state->io, [state, &stopped] {
        state->finish(false, "disconnected");
        state->alive.store(false);
        state->debounce.cancel();
        stopped.set_value();
    });
    stopped.get_future().wait();
    state->monitor.reset();
    state->disk.join();
    state->keep.reset();
    state->io.stop();
    state->thread.join();
}
void ClipboardManager::start() { state_->post([](State& self) { self.start(); }); }
void ClipboardManager::context(Context value) {
    {
        std::scoped_lock lock(state_->context_mutex);
        if (state_->submitted == value) return;
        state_->submitted = value;
    }
    state_->revision.fetch_add(1);
    // Invalidate queued native publication immediately, not after a UI/network round trip.
    if (auto guard = state_->publication.load()) guard->store(false);
    state_->post([value = std::move(value)](State& self) {
        if (self.context == value) return;
        self.finish(false, "focus_or_enablement_changed");
        self.context = value;
    });
}
void ClipboardManager::event(json::object message) {
    if (string_field(message, "type") == "clipboard_changed") {
        state_->revision.fetch_add(1);
        if (auto guard = state_->publication.load()) guard->store(false);
    }
    state_->post([message = std::move(message)](State& self) { self.event(message); });
}
void ClipboardManager::local_changed(ClipboardChange change) {
    state_->post([change](State& self) {
        if (change.excluded || (self.current && change.sequence != self.current->sequence)) self.finish(false, "source_changed");
        if (!change.excluded && self.context.enabled && self.context.active && change.sequence != self.uploaded) self.upload(false);
    });
}
void ClipboardManager::upload(bool manual) { state_->post([manual](State& self) { self.upload(manual); }); }
void ClipboardManager::download(bool manual) { state_->post([manual](State& self) { self.download(manual); }); }
void ClipboardManager::paste(std::function<void(bool)> completed) {
    state_->post([completed = std::move(completed)](State& self) mutable {
        if (!self.context.enabled && !self.current) { self.dispatch([completed] { completed(true); }); return; }
        const auto sequence = self.native.sequence();
        if (!self.current && self.uploaded == sequence) {
            const auto revision = self.revision.load();
            const auto foreground = self.context.foreground;
            self.dispatch([weak = self.weak_from_this(), completed = std::move(completed), sequence, revision, foreground] {
                const auto state = weak.lock();
                completed(state && state->alive.load() && state->revision.load() == revision &&
                    state->native.sequence() == sequence && (!foreground || GetForegroundWindow() == foreground));
            });
            return;
        }
        self.upload(false, std::move(completed));
    });
}
void ClipboardManager::cancel() {
    state_->revision.fetch_add(1);
    if (auto guard = state_->publication.load()) guard->store(false);
    state_->post([](State& self) { self.finish(false, "cancelled"); });
}
void ClipboardManager::policy(bulk::TransferPolicy policy) {
    state_->post([policy](State& self) {
        if (!bulk::valid_policy(policy)) { self.show("failed", true, "invalid_policy"); return; }
        self.policy = policy;
    });
}
} // namespace beamit::client
