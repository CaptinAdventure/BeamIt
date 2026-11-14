#include "host/wss/ControlSession.hpp"
#include "host/wss/HttpFrontend.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iostream>
#include <winioctl.h>

using namespace beamit;
using namespace beamit::host;
using namespace beamit::host::wss;
using namespace std::chrono_literals;
namespace bulk = beamit::net::clipboard_bulk;

namespace {

const std::string png_text =
"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl6VwsAAAAASUVORK5CYII=";
awaitable<void> reject(Server& server, wintls::context& tls, unsigned short port, std::string path);

struct TemporaryStaging {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("BeamItBulkWss-" + net::base64url_encode(net::random_bytes(12)));
    ~TemporaryStaging() { std::error_code error; std::filesystem::remove_all(path, error); }
};

awaitable<std::shared_ptr<WebSocket>> connect_socket(Server& server, wintls::context& tls, unsigned short port) {
    auto socket = std::make_shared<WebSocket>(server.io, tls);
    co_await beast::get_lowest_layer(*socket).async_connect(
        tcp::endpoint(asio::ip::address_v4::loopback(), port), use_awaitable);
    co_await socket->next_layer().async_handshake(wintls::handshake_type::client, use_awaitable);
    co_return socket;
}

awaitable<void> send(WebSocket& socket, json::object object) {
    const auto text = json::serialize(object);
    socket.text(true);
    co_await socket.async_write(asio::buffer(text), use_awaitable);
}

awaitable<json::object> read(WebSocket& socket, bool include_progress = false) {
    while (true) {
        beast::flat_buffer buffer;
        co_await socket.async_read(buffer, use_awaitable);
        auto object = json::parse(beast::buffers_to_string(buffer.data())).as_object();
        if (object.at("type") != "stream_stats" &&
            (include_progress || object.at("type") != "clipboard_bulk_progress")) co_return object;
    }
}

    namespace {
    struct SignalingMeasurement {
        Clock::time_point started;
        std::vector<double> ping_ms;
        double seconds{};
        std::uint64_t received{};
        std::ptrdiff_t owned{}, peak_owned{};
        int writes{}, peak_writes{};
    };

    enum class DownloadScenario { Mixed, Png, Legacy, Stale, ChangedSelection, Mutation, Excluded, Quota,
        Cancel, CancelPreparing, Disconnect, ReceiverFailure, EarlyComplete, Expired, Idle, Absolute, ChangedAfterOffer,
        ChangedBeforeEnd, ControlDisconnect, DiskFull, PermissionDenied, Responsiveness };

    awaitable<void> download(Server& server, unsigned short port, DownloadScenario scenario,
        const std::filesystem::path& source, std::atomic<DWORD>& sequence,
        std::atomic_bool& preparing, std::atomic_bool& release, SignalingMeasurement* measurement = nullptr) {
        wintls::context tls(wintls::method::system_default);
        tls.verify_server_certificate(false);
        auto control = co_await connect_socket(server, tls, port);
        co_await control->async_handshake("localhost", "/control", use_awaitable);
        const auto hello = co_await read(*control);
        const auto nonce = net::base64url_decode(hello.at("auth").as_object().at("server_nonce").as_string());
        REQUIRE(nonce);
        const auto client_nonce = net::random_bytes(32);
        co_await send(*control, {{"type", "authenticate"}, {"protocol", 1},
            {"client_nonce", net::base64url_encode(client_nonce)},
            {"proof", net::base64url_encode(net::auth_proof(server.options.auth_key, "client", *nonce, client_nonce))}});
        REQUIRE((co_await read(*control)).at("type") == "authenticated");
        if (scenario != DownloadScenario::Legacy) {
            co_await send(*control, {{"type", "clipboard_bulk_negotiate"}, {"id", 1}, {"version", 1}});
            REQUIRE((co_await read(*control)).at("type") == "clipboard_bulk_capabilities");
        }
        co_await send(*control, {{"type", "get_clipboard"}, {"id", 2}});
        const auto preliminary = co_await read(*control);
        REQUIRE(preliminary.at("type") == "clipboard");
        REQUIRE(preliminary.at("id") == 2);
        if (scenario == DownloadScenario::Legacy) {
            CHECK(preliminary.at("text") == "native text");
            CHECK(std::string(preliminary.at("image").as_object().at("data").as_string()) == png_text);
            CHECK_FALSE(preliminary.contains("manifest"));
            CHECK_FALSE(preliminary.contains("snapshot_id"));
            co_await send(*control, {{"type", "clipboard_bulk_request"}, {"id", 20}, {"snapshot_id", "unknown"}});
            CHECK((co_await read(*control)).at("code") == "not_ready");
        } else if (scenario == DownloadScenario::Excluded) {
            CHECK(preliminary.at("excluded") == true);
            CHECK_FALSE(preliminary.contains("manifest"));
            CHECK_FALSE(preliminary.contains("text"));
            CHECK_FALSE(preliminary.contains("snapshot_id"));
            CHECK_FALSE(std::filesystem::exists(server.bulk_staging_root / L"host" / L"outgoing"));
        } else {
            CHECK(preliminary.at("sequence") == 42);
            CHECK(preliminary.at("text") == "native text");
            CHECK(preliminary.at("html") == "<b>native text</b>");
            CHECK_FALSE(preliminary.at("image").as_object().contains("data"));
            CHECK_FALSE(preliminary.contains("url"));
            const auto& pre_manifest = preliminary.at("manifest").as_object();
            CHECK(pre_manifest.at("selected_roots") == (scenario == DownloadScenario::Png ? 0 : 1));
            CHECK(pre_manifest.at("roots").as_array().size() == (scenario == DownloadScenario::Png ? 0 : 1));
            for (const auto& node : pre_manifest.at("nodes").as_array()) CHECK_FALSE(node.as_object().contains("sha256"));
            if (scenario == DownloadScenario::Mixed) {
                co_await send(*control, {{"type", "clipboard_bulk_request"}, {"id", 20}, {"snapshot_id", "unknown"}});
                CHECK((co_await read(*control)).at("code") == "stale_snapshot");
            }
            if (scenario == DownloadScenario::Stale) sequence.store(43);
            if (scenario == DownloadScenario::ChangedSelection) {
                std::ofstream(source / L"selection" / L"new.txt") << "new selection";
            }
            co_await send(*control, {{"type", "clipboard_bulk_request"}, {"id", 3},
                {"snapshot_id", preliminary.at("snapshot_id")}});
            const auto preparing_reply = co_await read(*control, true);
            REQUIRE(preparing_reply.at("type") == "clipboard_bulk_progress");
            CHECK(preparing_reply.at("phase") == "preparing");
            CHECK(preparing_reply.at("id") == 3);
            CHECK_FALSE(preparing_reply.contains("url"));
            const auto transfer = preparing_reply.at("transfer_id");
            if (scenario == DownloadScenario::CancelPreparing) {
                while (!preparing.load()) {
                    asio::steady_timer pause(server.io, 1ms);
                    co_await pause.async_wait(use_awaitable);
                }
                co_await send(*control, {{"type", "clipboard_bulk_cancel"}, {"transfer_id", transfer}});
                CHECK((co_await read(*control)).at("type") == "clipboard_bulk_cancelled");
                release.store(true);
            } else {
                const auto offer = co_await read(*control);
                if (scenario == DownloadScenario::Stale || scenario == DownloadScenario::ChangedSelection ||
                    scenario == DownloadScenario::Mutation) {
                    REQUIRE(offer.at("type") == "clipboard_bulk_reconfirm");
                    CHECK(offer.at("transfer_id") == transfer);
                    CHECK(offer.at("snapshot_id") != preliminary.at("snapshot_id"));
                    CHECK_FALSE(offer.contains("url"));
                    if (scenario == DownloadScenario::ChangedSelection) {
                        CHECK(offer.at("manifest").as_object().at("nodes").as_array().size() ==
                            pre_manifest.at("nodes").as_array().size() + 1);
                    }
                } else if (scenario == DownloadScenario::Quota || scenario == DownloadScenario::DiskFull ||
                    scenario == DownloadScenario::PermissionDenied) {
                    CHECK(offer.at("type") == "clipboard_bulk_failed");
                    CHECK(offer.at("code") == (scenario == DownloadScenario::Quota ? "quota_exceeded" :
                        scenario == DownloadScenario::DiskFull ? "disk_full" : "permission_denied"));
                    CHECK_FALSE(offer.contains("url"));
                } else {
                    REQUIRE(offer.at("type") == "clipboard_bulk_offer");
                    CHECK(offer.at("transfer_id") == transfer);
                    CHECK(offer.at("direction") == "host_to_client");
                    CHECK(offer.at("source_snapshot_id") == preliminary.at("snapshot_id"));
                    CHECK_FALSE(offer.contains("id"));
                    const std::string url(offer.at("url").as_string());
                    const auto path = url.substr(url.find('/', 6));
                    const auto spool = server.bulk_staging_root / L"host" / L"outgoing" /
                        std::filesystem::path(std::string(transfer.as_string()));
                    CHECK(std::filesystem::is_directory(spool));
                    const auto parsed = bulk::parse_manifest(offer.at("manifest"), json::serialize(offer.at("manifest")).size());
                    REQUIRE(parsed);
                    CHECK(offer.at("roots") == pre_manifest.at("roots"));
                    if (scenario == DownloadScenario::ControlDisconnect) {
                        beast::error_code ignored;
                        co_await control->async_close(websocket::close_code::normal,
                            asio::redirect_error(use_awaitable, ignored));
                        co_return;
                    }
                    co_await reject(server, tls, port, path + "wrong");
                    if (scenario == DownloadScenario::Cancel) {
                        co_await send(*control, {{"type", "clipboard_bulk_cancel"}, {"transfer_id", transfer}});
                        CHECK((co_await read(*control)).at("type") == "clipboard_bulk_cancelled");
                        co_await reject(server, tls, port, path);
                    } else if (scenario == DownloadScenario::EarlyComplete) {
                        co_await send(*control, {{"type", "clipboard_bulk_complete"}, {"transfer_id", transfer}});
                        CHECK((co_await read(*control)).at("code") == "early_complete");
                    } else if (scenario == DownloadScenario::Expired) {
                        server.bulk_clock = [] { return Clock::now() + 10s; };
                        co_await reject(server, tls, port, path);
                        CHECK((co_await read(*control)).at("code") == "timeout");
                    } else if (scenario == DownloadScenario::ChangedAfterOffer) {
                        sequence.store(43);
                        co_await reject(server, tls, port, path);
                        CHECK((co_await read(*control)).at("code") == "source_changed");
                    } else {
                        auto socket = co_await connect_socket(server, tls, port);
                        if (measurement) measurement->started = Clock::now();
                        co_await socket->async_handshake("localhost", path, use_awaitable);
                        co_await reject(server, tls, port, path);
                        if (scenario == DownloadScenario::Disconnect) {
                            beast::error_code ignored;
                            beast::get_lowest_layer(*socket).close(ignored);
                            CHECK((co_await read(*control)).at("code") == "interrupted");
                        } else {
                            const auto decoded = net::base64url_decode(transfer.as_string());
                            REQUIRE(decoded);
                            std::array<std::byte, 16> raw{};
                            std::ranges::copy(*decoded, raw.begin());
                            bulk::FrameSequence frames(raw, parsed->nodes);
                            std::vector<std::vector<std::byte>> payloads(parsed->nodes.size());
                            bool changed{};
                            if (scenario == DownloadScenario::ChangedBeforeEnd) {
                                // Exceed the one-MiB burst: the sender must await throttling,
                                // so changing a source after its first chunk deterministically
                                // reaches the production pre-END identity check.
                                beast::flat_buffer first;
                                co_await socket->async_read(first, use_awaitable);
                                CHECK(socket->got_binary());
                                std::ofstream(source / L"selection" / L"item.txt", std::ios::app) << "changed";
                                changed = true;
                            }
                            if (changed) {
                                // Keep draining immutable spool frames so the mock receiver
                                // does not prevent the sender from reaching its END guard.
                                while (true) {
                                    beast::flat_buffer remaining;
                                    beast::error_code error;
                                    co_await socket->async_read(remaining, asio::redirect_error(use_awaitable, error));
                                    if (error) break;
                                    std::vector<std::byte> bytes(remaining.size());
                                    asio::buffer_copy(asio::buffer(bytes), remaining.data());
                                    const auto header = bulk::decode_header(bytes);
                                    REQUIRE(header);
                                    CHECK(header->kind != bulk::FrameKind::End);
                                }
                                const auto failed = co_await read(*control);
                                CHECK(failed.at("type") == "clipboard_bulk_failed");
                                CHECK(failed.at("code") == "source_changed");
                            } else {
                                auto last_ping = Clock::now();
                                std::int64_t ping_id = 100;
                                while (!frames.ended()) {
                                    beast::flat_buffer buffer;
                                    co_await socket->async_read(buffer, use_awaitable);
                                    CHECK(socket->got_binary());
                                    std::vector<std::byte> bytes(buffer.size());
                                    asio::buffer_copy(asio::buffer(bytes), buffer.data());
                                    const auto frame = frames.accept(bytes);
                                    REQUIRE(frame);
                                    if (frame->kind != bulk::FrameKind::End) {
                                        auto& collected = payloads[frame->node_index];
                                        collected.insert(collected.end(), bytes.begin() + bulk::frame_header_size, bytes.end());
                                        if (measurement) measurement->received += frame->payload_size;
                                    }
                                    if (measurement && !frames.ended() && Clock::now() - last_ping >= 50ms) {
                                        const auto started = Clock::now();
                                        co_await send(*control, {{"type", "ping"}, {"id", ++ping_id}});
                                        const auto pong = co_await read(*control);
                                        const auto elapsed = Clock::now() - started;
                                        REQUIRE(pong.at("type") == "pong"); // Also rejects premature completion.
                                        CHECK(pong.at("id") == ping_id);
                                        measurement->ping_ms.push_back(
                                            std::chrono::duration<double, std::milli>(elapsed).count());
                                        last_ping = Clock::now();
                                    }
                                }
                                if (measurement) measurement->seconds =
                                    std::chrono::duration<double>(Clock::now() - measurement->started).count();
                                for (std::size_t index = 0; index < parsed->nodes.size(); ++index) {
                                    const auto& node = parsed->nodes[index];
                                    if (node.kind == bulk::Node::Kind::Directory) continue;
                                    CHECK(net::sha256(payloads[index]) == node.sha256);
                                    CHECK(payloads[index].size() == node.size);
                                    if (node.kind == bulk::Node::Kind::Png) CHECK(payloads[index] == *net::base64_decode(png_text));
                                }
                                if (scenario == DownloadScenario::Mixed) {
                                    CHECK(std::ranges::any_of(parsed->nodes, [](const auto& node) {
                                        return node.path == "selection/empty" && node.kind == bulk::Node::Kind::Directory;
                                    }));
                                    CHECK(std::ranges::any_of(parsed->nodes, [](const auto& node) {
                                        return node.path == "selection/.hidden";
                                    }));
                                    CHECK(std::ranges::any_of(parsed->nodes, [](const auto& node) {
                                        return node.path == "selection/zero.txt" && node.size == 0;
                                    }));
                                }
                                co_await send(*control, {{"type", "ping"}, {"id", 4}});
                                CHECK((co_await read(*control)).at("type") == "pong"); // Not a success event.
                                CHECK(std::filesystem::is_directory(spool)); // END is not completion.
                                if (scenario == DownloadScenario::Idle || scenario == DownloadScenario::Absolute) {
                                    server.bulk_clock = [scenario] {
                                        return Clock::now() + (scenario == DownloadScenario::Idle ? 30s : 30min);
                                    };
                                    CHECK((co_await read(*control)).at("code") == "timeout");
                                } else if (scenario == DownloadScenario::ReceiverFailure) {
                                    co_await send(*control, {{"type", "clipboard_bulk_failed"}, {"transfer_id", transfer},
                                        {"code", "clipboard_unavailable"}});
                                } else {
                                    co_await send(*control, {{"type", "clipboard_bulk_progress"}, {"transfer_id", transfer},
                                        {"phase", "receiving"}, {"completed_bytes", bulk::payload_bytes(*parsed)},
                                        {"total_bytes", bulk::payload_bytes(*parsed)},
                                        {"completed_nodes", parsed->nodes.size()}, {"total_nodes", parsed->nodes.size()}});
                                    co_await send(*control, {{"type", "clipboard_bulk_complete"}, {"transfer_id", transfer},
                                        {"sequence", 99}});
                                    co_await send(*control, {{"type", "clipboard_bulk_complete"}, {"transfer_id", transfer},
                                        {"sequence", 99}});
                                }
                            }
                        }
                    }
                }
            }
            co_await send(*control, {{"type", "ping"}, {"id", 5}});
            CHECK((co_await read(*control)).at("type") == "pong");
            const auto deadline = Clock::now() + 3s;
            const auto outgoing = server.bulk_staging_root / L"host" / L"outgoing";
            const auto ledgers = server.bulk_staging_root / L".reservations";
            while (((std::filesystem::exists(outgoing) && !std::filesystem::is_empty(outgoing)) ||
                    (std::filesystem::exists(ledgers) && !std::filesystem::is_empty(ledgers))) &&
                   Clock::now() < deadline) {
                asio::steady_timer pause(server.io, 1ms);
                co_await pause.async_wait(use_awaitable);
            }
            CHECK((!std::filesystem::exists(outgoing) || std::filesystem::is_empty(outgoing)));
            CHECK((!std::filesystem::exists(ledgers) || std::filesystem::is_empty(ledgers)));
        }
        beast::error_code ignored;
        co_await control->async_close(websocket::close_code::normal, asio::redirect_error(use_awaitable, ignored));
    }
    } // namespace

    void run_download(DownloadScenario scenario);

    TEST_CASE("Authenticated bulk downloads spool and stream the negotiated native snapshot", "[clipboard-bulk-download]") {
        auto scenario = DownloadScenario::Mixed;
        SECTION("selected folder hidden and empty entries with PNG await receiver acknowledgement") {}
        SECTION("PNG alone has no selected file roots") { scenario = DownloadScenario::Png; }
        SECTION("old peers retain the legacy PNG control response") { scenario = DownloadScenario::Legacy; }
        SECTION("stale clipboard generation requires a new snapshot") { scenario = DownloadScenario::Stale; }
        SECTION("selection changes require fresh preliminary confirmation") { scenario = DownloadScenario::ChangedSelection; }
        SECTION("source mutation during preparation discards the spool") { scenario = DownloadScenario::Mutation; }
        SECTION("excluded clipboard never enumerates even invalid source references") { scenario = DownloadScenario::Excluded; }
        SECTION("shared quota prevents outgoing spool admission") { scenario = DownloadScenario::Quota; }
        SECTION("cancel invalidates an offered socket and deletes spool") { scenario = DownloadScenario::Cancel; }
        SECTION("cancel reaches the background preparation") { scenario = DownloadScenario::CancelPreparing; }
        SECTION("bulk disconnect deletes sender-only content") { scenario = DownloadScenario::Disconnect; }
        SECTION("receiver publication failure is not completion") { scenario = DownloadScenario::ReceiverFailure; }
        SECTION("premature receiver success is rejected") { scenario = DownloadScenario::EarlyComplete; }
        SECTION("outgoing token expires after ten seconds") { scenario = DownloadScenario::Expired; }
        SECTION("missing receiver acknowledgement hits idle timeout") { scenario = DownloadScenario::Idle; }
        SECTION("absolute deadline remains active after sending END") { scenario = DownloadScenario::Absolute; }
        SECTION("source sequence checked again before claiming token") { scenario = DownloadScenario::ChangedAfterOffer; }
        SECTION("source file changes before END abort handoff") { scenario = DownloadScenario::ChangedBeforeEnd; }
        SECTION("control disconnect releases an unclaimed outgoing spool") { scenario = DownloadScenario::ControlDisconnect; }
        run_download(scenario);
    }

    TEST_CASE("Outgoing disk and permission failures surface through authenticated routes",
              "[clipboard-bulk-storage-wss]") {
        SECTION("disk full") { run_download(DownloadScenario::DiskFull); }
        SECTION("permission denied") { run_download(DownloadScenario::PermissionDenied); }
    }

    TEST_CASE("32 MiB loopback bulk keeps local control responsive with bounded application payload",
              "[clipboard-bulk-responsiveness]") {
        run_download(DownloadScenario::Responsiveness);
    }

    void run_download(DownloadScenario scenario) {
        HostServerOptions options;
        try { options.identity = installed_identity(); }
        catch (const MissingTlsCertificate&) { SKIP("Local TLS identity unavailable; install the existing host certificate"); }
        options.auth_salt = net::random_bytes(16);
        options.auth_iterations = 1;
        TemporaryStaging staging, source;
        std::filesystem::create_directories(source.path / L"selection" / L"empty");
        std::ofstream(source.path / L"selection" / L"item.txt", std::ios::binary) <<
            (scenario == DownloadScenario::Responsiveness ? std::string(32 * 1024 * 1024, 'x') :
                scenario == DownloadScenario::ChangedBeforeEnd ? std::string(2 * 1024 * 1024, 'x') : "immutable test payload");
        std::ofstream(source.path / L"selection" / L".hidden") << "hidden";
        REQUIRE(SetFileAttributesW((source.path / L"selection" / L".hidden").c_str(), FILE_ATTRIBUTE_HIDDEN));
        std::ofstream(source.path / L"selection" / L"zero.txt");
        SignalingMeasurement measurement; // Outlives pending coroutine destruction in Server::io.
        Server server(std::move(options));
        server.bulk_staging_root = staging.path;
        std::atomic<DWORD> sequence{42};
        std::atomic_bool preparing{}, release{};
        if (scenario == DownloadScenario::Responsiveness) {
            server.bulk_send_activity = [&](std::ptrdiff_t bytes, int writes) {
                measurement.owned += bytes;
                measurement.writes += writes;
                measurement.peak_owned = std::max(measurement.peak_owned, measurement.owned);
                measurement.peak_writes = std::max(measurement.peak_writes, measurement.writes);
            };
        }
        if (scenario == DownloadScenario::DiskFull || scenario == DownloadScenario::PermissionDenied) {
            server.bulk_write_check = [scenario]() -> DWORD {
                return scenario == DownloadScenario::DiskFull ? ERROR_DISK_FULL : ERROR_ACCESS_DENIED;
            };
        }
        server.clipboard_sequence = [&] { return sequence.load(); };
        server.clipboard_read = [&](unsigned formats, bool binary, auto done) {
            CHECK(binary == (scenario != DownloadScenario::Legacy));
            CHECK(bool(formats & clipboard_files) == binary);
            ClipboardContent content{.sequence = sequence.load(), .text = "native text",
                .html = "<b>native text</b>",
                .image = ClipboardImage{.png = *net::base64_decode(png_text), .width = 1, .height = 1}};
            if (binary && scenario != DownloadScenario::Png) content.files.push_back(source.path / L"selection");
            if (scenario == DownloadScenario::Excluded) {
                content.excluded = true;
                content.files = {source.path / L"must-never-enumerate"};
            }
            done({.content = std::move(content)});
        };
        server.clipboard_publish = [](ClipboardWrite, auto) { FAIL("download must never write the host clipboard"); };
        server.bulk_prepare_observer = [&](std::uint64_t) {
            if (preparing.exchange(true)) return;
            if (scenario == DownloadScenario::Mutation) {
                std::ofstream(source.path / L"selection" / L"item.txt", std::ios::app) << "changed";
            }
            if (scenario == DownloadScenario::CancelPreparing) {
                while (!release.load()) Sleep(1);
            }
        };
        if (scenario == DownloadScenario::Quota) {
            std::filesystem::create_directories(staging.path / L"client" / L"committed");
            const auto full = staging.path / L"client" / L"committed" / L"retained";
            const auto sparse = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
            REQUIRE(sparse != INVALID_HANDLE_VALUE);
            DWORD returned{};
            const bool marked = DeviceIoControl(sparse, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
            CloseHandle(sparse);
            REQUIRE(marked);
            std::filesystem::resize_file(full, 7'900'000'000);
        }
        server.tls.use_certificate(server.options.identity.certificate.get());
        tcp::acceptor acceptor(server.io, {asio::ip::address_v4::loopback(), 0});
        asio::co_spawn(server.io, accept_loop(server, acceptor), asio::detached);
        bool completed{}, timed_out{};
        std::exception_ptr failure;
        asio::steady_timer timeout(server.io, 20s);
        timeout.async_wait([&](beast::error_code error) {
            if (!error) { timed_out = true; release.store(true); server.io.stop(); }
        });
        asio::co_spawn(server.io, download(server, acceptor.local_endpoint().port(), scenario, source.path,
            sequence, preparing, release, scenario == DownloadScenario::Responsiveness ? &measurement : nullptr),
            [&](std::exception_ptr error) {
                failure = error;
                completed = true;
                release.store(true);
                timeout.cancel();
                acceptor.close();
                if (auto session = server.active.lock()) session->close(websocket::close_code::going_away, "test ended");
                server.io.stop();
            });
        server.io.run();
        server.bulk_worker.join();
        REQUIRE_FALSE(timed_out);
        REQUIRE(completed);
        if (failure) std::rethrow_exception(failure);
        const auto outgoing = staging.path / L"host" / L"outgoing";
        const auto ledgers = staging.path / L".reservations";
        CHECK((!std::filesystem::exists(outgoing) || std::filesystem::is_empty(outgoing)));
        CHECK((!std::filesystem::exists(ledgers) || std::filesystem::is_empty(ledgers)));
        if (scenario == DownloadScenario::Responsiveness) {
            REQUIRE(measurement.ping_ms.size() >= 20);
            std::ranges::sort(measurement.ping_ms);
            const auto maximum = measurement.ping_ms.back();
            const auto p95 = measurement.ping_ms[(measurement.ping_ms.size() - 1) * 95 / 100];
            std::cout << "LOCAL_SIGNALING_ONLY bytes=" << measurement.received << " seconds=" << measurement.seconds
                << " pings=" << measurement.ping_ms.size() << " p95_ms=" << p95 << " max_ms=" << maximum
                << " peak_owned_payload=" << measurement.peak_owned << " peak_writes=" << measurement.peak_writes << '\n';
            CHECK(measurement.received >= 32 * 1024 * 1024);
            CHECK(measurement.seconds >= 3.0); // 8 MiB/s with one MiB burst, not an unthrottled loopback copy.
            CHECK(maximum < 1000.0); // Local fixture guard, not a video/input or LAN performance SLO.
            CHECK(measurement.peak_owned > 0);
            CHECK(measurement.peak_owned <= 1024 * 1024);
            CHECK(measurement.peak_writes == 1);
            CHECK(measurement.owned == 0);
            CHECK(measurement.writes == 0);
        }
    }

awaitable<void> reject(Server& server, wintls::context& tls, unsigned short port, std::string path) {
    auto socket = co_await connect_socket(server, tls, port);
    websocket::response_type response;
    beast::error_code error;
    co_await socket->async_handshake(response, "localhost", path, asio::redirect_error(use_awaitable, error));
    CHECK(error == websocket::error::upgrade_declined);
    CHECK(response.result() == http::status::forbidden);
}

enum class Scenario { EndFirst, CommitFirst, Quota, PublicationFailure, Stale, Cancel, BadDigest,
                      MixedPng, PngOnly, WrongCommit, ExtraEnd, ExpiredToken, IdleTimeout, AbsoluteTimeout,
                      PasswordChanged, Disconnect, TextFrame, DelayedPublication, CancelPublishing, RollbackFailure,
                      DiskFull, PermissionDenied };

awaitable<void> exercise(Server& server, unsigned short port, Scenario scenario, unsigned& publications,
                         std::function<void()>& deferred) {
    wintls::context tls(wintls::method::system_default);
    tls.verify_server_certificate(false); // Loopback test only; no production trust changes.
    auto control = co_await connect_socket(server, tls, port);
    co_await control->async_handshake("localhost", "/control", use_awaitable);
    const auto hello = co_await read(*control);
    const auto nonce = net::base64url_decode(hello.at("auth").as_object().at("server_nonce").as_string());
    REQUIRE(nonce);
    const auto client_nonce = net::random_bytes(32);
    co_await send(*control, {{"type", "authenticate"}, {"protocol", 1},
        {"client_nonce", net::base64url_encode(client_nonce)},
        {"proof", net::base64url_encode(net::auth_proof(server.options.auth_key, "client", *nonce, client_nonce))}});
    REQUIRE((co_await read(*control)).at("type") == "authenticated");
    co_await send(*control, {{"type", "clipboard_bulk_begin"}, {"id", 0}});
    CHECK((co_await read(*control)).at("code") == "not_ready");
    co_await send(*control, {{"type", "clipboard_bulk_negotiate"}, {"id", 1}, {"version", 1}});
    const auto capabilities = co_await read(*control);
    REQUIRE(capabilities.at("type") == "clipboard_bulk_capabilities");
    CHECK(capabilities.at("id") == 1);
    CHECK(capabilities.at("max_file_bytes") == 2'000'000'000);

    const std::string content = "immutable test payload";
    const auto bytes = std::as_bytes(std::span(content));
    bulk::Manifest manifest{.selected_roots = 1, .nodes = {
        {.kind = bulk::Node::Kind::Directory, .path = "selection"},
        {.kind = bulk::Node::Kind::File, .path = "selection/item.txt", .size = bytes.size(),
         .sha256 = net::sha256(bytes)},
        {.kind = bulk::Node::Kind::Directory, .path = "selection/empty"}}};
    if (scenario == Scenario::BadDigest) manifest.nodes[1].sha256 = {};
    const auto png = net::base64_decode(png_text);
    REQUIRE(png);
    if (scenario == Scenario::PngOnly) manifest = {};
    if (scenario == Scenario::MixedPng || scenario == Scenario::PngOnly) {
        manifest.nodes.push_back({.kind = bulk::Node::Kind::Png, .path = "clipboard.png",
            .size = png->size(), .sha256 = net::sha256(*png), .width = 1, .height = 1});
    }
    co_await send(*control, {{"type", "clipboard_bulk_begin"}, {"id", 2},
        {"source_snapshot_id", "source-revision"}, {"manifest", bulk::manifest_json(manifest)},
        {"text", "accompanying text"}, {"html", "<b>accompanying text</b>"}, {"sensitive", true}});
    auto offer = co_await read(*control);
    if (scenario == Scenario::Quota) {
        REQUIRE(offer.at("type") == "error");
        CHECK(offer.at("code") == "quota_exceeded");
        CHECK((co_await read(*control)).at("type") == "clipboard_bulk_failed");
        CHECK(publications == 0);
    } else {
        REQUIRE(offer.at("type") == "clipboard_bulk_offer");
        CHECK(offer.at("id") == 2);
        const std::string url(offer.at("url").as_string());
        const auto path = url.substr(url.find('/', 6));
        co_await reject(server, tls, port, path + "wrong");
        if (scenario == Scenario::ExpiredToken) {
            server.bulk_clock = [] { return Clock::now() + 10s; };
            co_await reject(server, tls, port, path);
            CHECK((co_await read(*control)).at("code") == "timeout");
            CHECK(publications == 0);
            beast::error_code ignored;
            co_await control->async_close(websocket::close_code::normal,
                asio::redirect_error(use_awaitable, ignored));
            co_return;
        }
        auto socket = co_await connect_socket(server, tls, port);
        co_await socket->async_handshake("localhost", path, use_awaitable);
        co_await reject(server, tls, port, path); // Single-use claim.
        if (scenario == Scenario::IdleTimeout || scenario == Scenario::AbsoluteTimeout ||
            scenario == Scenario::PasswordChanged || scenario == Scenario::Disconnect ||
            scenario == Scenario::TextFrame) {
            if (scenario == Scenario::IdleTimeout) server.bulk_clock = [] { return Clock::now() + 30s; };
            if (scenario == Scenario::AbsoluteTimeout) server.bulk_clock = [] { return Clock::now() + 30min; };
            if (scenario == Scenario::PasswordChanged) {
                HostCommand change{.kind = HostCommand::Kind::ChangePassword};
                server.change_password(change);
            }
            if (scenario == Scenario::Disconnect) {
                beast::error_code ignored;
                beast::get_lowest_layer(*socket).close(ignored);
            }
            if (scenario == Scenario::TextFrame) {
                socket->text(true);
                co_await socket->async_write(asio::buffer(std::string("not binary")), use_awaitable);
            }
            const auto failed = co_await read(*control);
            CHECK(failed.at("type") == "clipboard_bulk_failed");
            CHECK(failed.at("code") == (scenario == Scenario::PasswordChanged ? "session_ended" :
                scenario == Scenario::Disconnect ? "interrupted" :
                scenario == Scenario::TextFrame ? "invalid_frame" : "timeout"));
            CHECK(publications == 0);
            beast::error_code ignored;
            co_await control->async_close(websocket::close_code::normal,
                asio::redirect_error(use_awaitable, ignored));
            co_return;
        }
        const auto transfer = offer.at("transfer_id");
        auto decoded = net::base64url_decode(transfer.as_string());
        REQUIRE(decoded);
        std::array<std::byte, 16> raw{};
        std::ranges::copy(*decoded, raw.begin());
        const auto commit = json::object{{"type", "clipboard_bulk_commit"}, {"id", 3},
            {"transfer_id", transfer}, {"source_snapshot_id", "source-revision"}};
        if (scenario == Scenario::CommitFirst) {
            co_await send(*control, commit);
            CHECK((co_await read(*control)).at("type") == "clipboard_bulk_committed");
            CHECK(publications == 0);
        }
        if (scenario == Scenario::Cancel) {
            co_await send(*control, {{"type", "clipboard_bulk_cancel"}, {"transfer_id", transfer}});
            CHECK((co_await read(*control)).at("type") == "clipboard_bulk_cancelled");
            CHECK(publications == 0);
        } else {
            socket->binary(true);
            for (std::uint32_t index = 0; index < manifest.nodes.size(); ++index) {
                const auto kind = manifest.nodes[index].kind;
                if (kind == bulk::Node::Kind::Directory) continue;
                const std::span<const std::byte> payload = kind == bulk::Node::Kind::Png ?
                    std::span<const std::byte>(*png) : bytes;
                const auto header = bulk::encode_header({
                    .kind = kind == bulk::Node::Kind::Png ? bulk::FrameKind::Png : bulk::FrameKind::File,
                    .transfer_id = raw, .node_index = index,
                    .payload_size = static_cast<std::uint32_t>(payload.size())});
                std::vector<std::byte> frame(header.begin(), header.end());
                frame.insert(frame.end(), payload.begin(), payload.end());
                co_await socket->async_write(asio::buffer(frame), use_awaitable);
            }
            if (scenario == Scenario::BadDigest || scenario == Scenario::DiskFull || scenario == Scenario::PermissionDenied) {
                const auto failed = co_await read(*control);
                CHECK(failed.at("type") == "clipboard_bulk_failed");
                CHECK(failed.at("code") == (scenario == Scenario::BadDigest ? "digest_mismatch" :
                    scenario == Scenario::DiskFull ? "disk_full" : "permission_denied"));
                CHECK(publications == 0);
            } else {
                const auto end = bulk::encode_header({.kind = bulk::FrameKind::End, .transfer_id = raw,
                    .node_index = UINT32_MAX, .offset = bulk::payload_bytes(manifest)});
                co_await socket->async_write(asio::buffer(end), use_awaitable);
                if (scenario == Scenario::ExtraEnd || scenario == Scenario::WrongCommit) {
                    if (scenario == Scenario::ExtraEnd) {
                        co_await socket->async_write(asio::buffer(end), use_awaitable);
                    } else {
                        co_await send(*control, {{"type", "clipboard_bulk_commit"}, {"transfer_id", transfer},
                            {"source_snapshot_id", "different-source"}});
                    }
                    CHECK((co_await read(*control)).at("type") == "clipboard_bulk_failed");
                    if (scenario == Scenario::WrongCommit) CHECK((co_await read(*control)).at("type") == "error");
                    CHECK(publications == 0);
                    beast::error_code ignored;
                    co_await control->async_close(websocket::close_code::normal,
                        asio::redirect_error(use_awaitable, ignored));
                    co_return;
                }
                if (scenario != Scenario::CommitFirst) {
                    // Control ordering is independent. A ping establishes no publication
                    // is possible without commit even if the worker has drained END.
                    co_await send(*control, {{"type", "ping"}, {"id", 4}});
                    CHECK((co_await read(*control)).at("type") == "pong");
                    CHECK(publications == 0);
                    if (scenario == Scenario::Stale) server.clipboard_sequence = [] { return DWORD(43); };
                    co_await send(*control, commit);
                    CHECK((co_await read(*control)).at("type") == "clipboard_bulk_committed");
                }
                if (scenario == Scenario::DelayedPublication || scenario == Scenario::CancelPublishing) {
                    while (!deferred) {
                        asio::steady_timer pause(server.io, 1ms);
                        co_await pause.async_wait(use_awaitable);
                    }
                    co_await send(*control, {{"type", "ping"}, {"id", 5}});
                    CHECK((co_await read(*control)).at("type") == "pong"); // Not a premature complete.
                    CHECK(publications == 0);
                    if (scenario == Scenario::CancelPublishing) {
                        co_await send(*control, {{"type", "clipboard_bulk_cancel"}, {"transfer_id", transfer}});
                        CHECK((co_await read(*control)).at("type") == "clipboard_bulk_cancelled");
                    }
                    auto finish = std::move(deferred);
                    finish();
                    if (scenario == Scenario::CancelPublishing) {
                        co_await send(*control, {{"type", "ping"}, {"id", 6}});
                        CHECK((co_await read(*control)).at("type") == "pong");
                        CHECK(publications == 0);
                        beast::error_code ignored;
                        co_await control->async_close(websocket::close_code::normal,
                            asio::redirect_error(use_awaitable, ignored));
                        co_return;
                    }
                }
                const auto terminal = co_await read(*control);
                if (scenario == Scenario::PublicationFailure || scenario == Scenario::RollbackFailure ||
                    scenario == Scenario::Stale) {
                    CHECK(terminal.at("type") == "clipboard_bulk_failed");
                    CHECK(terminal.at("code") ==
                        (scenario == Scenario::Stale ? "stale_clipboard" : "clipboard_unavailable"));
                    CHECK(publications == 0);
                } else {
                    CHECK(terminal.at("type") == "clipboard_bulk_complete");
                    CHECK(terminal.at("transfer_id") == transfer);
                    CHECK(terminal.at("sequence") == 43);
                    CHECK(publications == 1);
                    co_await send(*control, commit);
                    CHECK((co_await read(*control)).at("type") == "clipboard_bulk_committed");
                }
            }
        }
    }
    beast::error_code ignored;
    co_await control->async_close(websocket::close_code::normal, asio::redirect_error(use_awaitable, ignored));
}
} // namespace

namespace {
void run_upload(Scenario scenario);
}
TEST_CASE("Authenticated bulk uploads use production route storage barrier and publication", "[clipboard-bulk-wss]") {
    auto scenario = Scenario::EndFirst;
    SECTION("END then commit publishes") {}
    SECTION("commit then END publishes") { scenario = Scenario::CommitFirst; }
    SECTION("shared quota prevents an offer") { scenario = Scenario::Quota; }
    SECTION("native publication failure never acknowledges success") { scenario = Scenario::PublicationFailure; }
    SECTION("destination revision changes prevent publication") { scenario = Scenario::Stale; }
    SECTION("cancel invalidates upload") { scenario = Scenario::Cancel; }
    SECTION("digest failure prevents publication") { scenario = Scenario::BadDigest; }
    SECTION("mixed PNG and files are prepared together") { scenario = Scenario::MixedPng; }
    SECTION("PNG-only snapshot uses binary publication") { scenario = Scenario::PngOnly; }
    SECTION("mismatched commit fails the transfer") { scenario = Scenario::WrongCommit; }
    SECTION("additional END before commit is rejected") { scenario = Scenario::ExtraEnd; }
    SECTION("single-use token expires at ten seconds") { scenario = Scenario::ExpiredToken; }
    SECTION("thirty seconds without progress cancels receipt") { scenario = Scenario::IdleTimeout; }
    SECTION("absolute thirty minute deadline cancels receipt") { scenario = Scenario::AbsoluteTimeout; }
    SECTION("password generation change invalidates receipt") { scenario = Scenario::PasswordChanged; }
    SECTION("bulk disconnect without END fails") { scenario = Scenario::Disconnect; }
    SECTION("text frames are rejected") { scenario = Scenario::TextFrame; }
    SECTION("completion waits for native publication callback") { scenario = Scenario::DelayedPublication; }
    SECTION("cancel while native publication is queued prevents replacement") { scenario = Scenario::CancelPublishing; }
    SECTION("failed native rollback retains potentially referenced files without success") {
        scenario = Scenario::RollbackFailure;
    }
    run_upload(scenario);
}

TEST_CASE("Incoming disk and permission failures never publish or acknowledge success",
          "[clipboard-bulk-storage-wss]") {
    SECTION("disk full") { run_upload(Scenario::DiskFull); }
    SECTION("permission denied") { run_upload(Scenario::PermissionDenied); }
}

namespace {
void run_upload(Scenario scenario) {
    HostServerOptions options;
    try { options.identity = installed_identity(); }
    catch (const MissingTlsCertificate&) { SKIP("Local TLS identity unavailable; install the existing host certificate"); }
    options.auth_salt = net::random_bytes(16);
    options.auth_iterations = 1;
    TemporaryStaging staging;
    Server server(std::move(options));
    server.bulk_staging_root = staging.path;
    if (scenario == Scenario::DiskFull || scenario == Scenario::PermissionDenied) {
        server.bulk_write_check = [scenario]() -> DWORD {
            return scenario == Scenario::DiskFull ? ERROR_DISK_FULL : ERROR_ACCESS_DENIED;
        };
    }
    server.clipboard_sequence = [] { return DWORD(42); };
    unsigned publications{};
    std::function<void()> deferred;
    std::filesystem::path published;
    server.clipboard_publish = [&](ClipboardWrite write, auto done) {
        CHECK(write.expected_sequence == 42);
        CHECK(write.publication_allowed->load());
        REQUIRE(write.files.size() == (scenario == Scenario::PngOnly ? 0 : 1));
        CHECK(write.sensitive);
        CHECK(write.text == "accompanying text");
        CHECK(write.html == "<b>accompanying text</b>");
        if (!write.files.empty()) {
            std::ifstream input(write.files[0] / L"item.txt", std::ios::binary);
            CHECK(std::string(std::istreambuf_iterator<char>(input), {}) == "immutable test payload");
            CHECK(std::filesystem::is_directory(write.files[0] / L"empty"));
        }
        if (scenario == Scenario::MixedPng || scenario == Scenario::PngOnly) {
            REQUIRE(write.png);
            CHECK(write.png == net::base64_decode(png_text));
        }
        if (scenario == Scenario::DelayedPublication || scenario == Scenario::CancelPublishing) {
            deferred = [&, allowed = write.publication_allowed, files = write.files, done = std::move(done)]() mutable {
                if (!allowed->load()) done({.error_code = "stale_clipboard", .error = "cancelled before native commit"});
                else {
                    ++publications;
                    published = files.front();
                    done({.sequence = 43});
                }
            };
        } else if (scenario == Scenario::RollbackFailure) {
            published = write.files[0];
            done({.error_code = "clipboard_unavailable", .error = "injected rollback failure", .retain_files = true});
        } else if (scenario == Scenario::PublicationFailure) {
            done({.error_code = "clipboard_unavailable", .error = "injected native failure"});
        } else {
            ++publications;
            if (!write.files.empty()) published = write.files[0];
            done({.sequence = 43});
        }
    };
    if (scenario == Scenario::Quota) {
        std::filesystem::create_directories(staging.path / L"client" / L"committed");
        const auto full = staging.path / L"client" / L"committed" / L"retained";
        const auto sparse = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        REQUIRE(sparse != INVALID_HANDLE_VALUE);
        DWORD returned{};
        const bool marked = DeviceIoControl(sparse, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
        CloseHandle(sparse);
        REQUIRE(marked);
        std::filesystem::resize_file(full, 7'900'000'000);
    }
    server.tls.use_certificate(server.options.identity.certificate.get());
    tcp::acceptor acceptor(server.io, {asio::ip::address_v4::loopback(), 0});
    asio::co_spawn(server.io, accept_loop(server, acceptor), asio::detached);
    bool completed{}, timed_out{};
    std::exception_ptr failure;
    asio::steady_timer timeout(server.io, 15s);
    timeout.async_wait([&](beast::error_code error) {
        if (!error) { timed_out = true; server.io.stop(); }
    });
    asio::co_spawn(server.io, exercise(server, acceptor.local_endpoint().port(), scenario, publications, deferred),
        [&](std::exception_ptr error) {
            failure = error;
            completed = true;
            timeout.cancel();
            acceptor.close();
            if (auto session = server.active.lock()) session->close(websocket::close_code::going_away, "test ended");
            server.io.stop();
        });
    server.io.run();
    server.bulk_worker.join();
    REQUIRE_FALSE(timed_out);
    REQUIRE(completed);
    if (failure) std::rethrow_exception(failure);
    if (publications && !published.empty()) CHECK(std::filesystem::exists(published / L"item.txt"));
    if (scenario == Scenario::RollbackFailure) CHECK(std::filesystem::exists(published / L"item.txt"));
    if (std::filesystem::exists(staging.path / L"host" / L"partial")) {
        CHECK(std::filesystem::is_empty(staging.path / L"host" / L"partial"));
    }
    if (scenario == Scenario::PublicationFailure || scenario == Scenario::Stale ||
        scenario == Scenario::CancelPublishing || scenario == Scenario::DiskFull || scenario == Scenario::PermissionDenied) {
        CHECK(std::filesystem::is_empty(staging.path / L"host" / L"committed"));
    }
    if (scenario == Scenario::DiskFull || scenario == Scenario::PermissionDenied) {
        CHECK(publications == 0);
        CHECK(std::filesystem::is_empty(staging.path / L".reservations"));
    }
}
} // namespace
