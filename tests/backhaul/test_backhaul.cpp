#include "Backhaul.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace backhaul;
using Bytes = std::vector<uint8_t>;
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + " " #expr); } while (false)

static Bytes encoded(const Frame &frame) {
    Bytes out(kMaxWireSize);
    const std::size_t size = encode(frame, out.data(), out.size());
    CHECK(size);
    out.resize(size);
    return out;
}
static Frame decoded(const Bytes &bytes) {
    CHECK(!bytes.empty());
    Decoder decoder;
    Frame frame;
    for (std::size_t i = 0; i < bytes.size(); ++i)
        CHECK(decoder.feed(bytes[i], frame) ==
              (i + 1 == bytes.size() ? Decoder::Step::Complete : Decoder::Step::More));
    return frame;
}
static Bytes next(Endpoint &endpoint) {
    std::size_t size = 0;
    const uint8_t *p = endpoint.output(size);
    if (!size) return {};
    Bytes bytes(p, p + size);
    CHECK(endpoint.consumeOutput(size));
    return bytes;
}
static bool deliver(Endpoint &to, const Bytes &bytes, uint64_t now, std::size_t chunk = 7) {
    for (std::size_t i = 0; i < bytes.size(); i += chunk)
        if (!to.input(bytes.data() + i, std::min(chunk, bytes.size() - i), now)) return false;
    return true;
}
static void pump(Endpoint &a, Endpoint &b, uint64_t now, std::size_t chunk = 7) {
    for (unsigned guard = 0; guard < 100; ++guard) {
        Bytes ab = next(a), ba = next(b);
        if (ab.empty() && ba.empty()) return;
        CHECK(deliver(b, ab, now, chunk));
        CHECK(deliver(a, ba, now, chunk));
    }
    CHECK(false);
}
struct Pair {
    Endpoint a{1, 2}, b{2, 1};
    Pair() {
        CHECK(a.open(10, 0)); CHECK(b.open(20, 0)); pump(a, b, 0);
        CHECK(a.state() == State::Ready && b.state() == State::Ready);
    }
};
static uint32_t send(Endpoint &from, const Bytes &bytes, uint64_t now = 0) {
    uint32_t sequence = 0;
    CHECK(from.submit(bytes.data(), bytes.size(), now, sequence) == Submit::Queued);
    CHECK(sequence);
    return sequence;
}
struct Radio {
    std::size_t capacity = 4;
    std::vector<Bytes> queued;
    std::size_t attempts = 0;
    static bool admit(const Frame &frame, void *context) {
        auto &radio = *static_cast<Radio *>(context);
        ++radio.attempts;
        if (radio.queued.size() == radio.capacity) return false;
        radio.queued.emplace_back(frame.payload.begin(), frame.payload.begin() + frame.length);
        return true;
    }
    void service(Endpoint &endpoint, uint64_t now = 0) {
        while (endpoint.serviceIncoming(admit, this, now)) {}
    }
};
static Result result(Endpoint &endpoint, Delivery expected) {
    Result r;
    CHECK(endpoint.takeResult(r)); CHECK(r.delivery == expected);
    return r;
}
static Frame dataFrame(uint32_t seq = 1) {
    Frame frame;
    frame.type = Type::Data; frame.node = 1; frame.epoch = 10;
    frame.targetEpoch = 20; frame.sequence = seq; frame.length = 3;
    frame.payload[0] = 'a'; frame.payload[1] = 'b'; frame.payload[2] = 'c';
    return frame;
}
static Bytes unhex(const std::string &hex) {
    Bytes out;
    for (std::size_t i = 0; i < hex.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

static void golden_and_all_payload_lengths() {
    Frame f = dataFrame();
    f.node = 0x01020304; f.epoch = 0x0102030405060708ULL;
    f.targetEpoch = 0x1112131415161718ULL; f.sequence = 0x11223344;
    // Independently generated with Python struct.pack('!2sBBHHIQQI') + zlib.crc32.
    CHECK(encoded(f) == unhex("4342010300030000010203040102030405060708"
                             "1112131415161718112233446162639b8e7d5a"));
    for (uint16_t length = 1; length <= kMaxPayload; ++length) {
        f.length = length;
        for (uint16_t i = 0; i < length; ++i) f.payload[i] = static_cast<uint8_t>(i * 37);
        const Frame copy = decoded(encoded(f));
        CHECK(copy.node == f.node && copy.epoch == f.epoch && copy.targetEpoch == f.targetEpoch);
        CHECK(copy.sequence == f.sequence && copy.length == length);
        CHECK(std::equal(copy.payload.begin(), copy.payload.begin() + length, f.payload.begin()));
    }
    uint8_t buffer[kMaxWireSize];
    CHECK(encode(f, buffer, kMaxWireSize - 1) == 0);
    f.length = kMaxPayload + 1;
    CHECK(encode(f, buffer, sizeof(buffer)) == 0);
    f.length = 0;
    CHECK(encode(f, buffer, sizeof(buffer)) == 0);
}

static void fragmentation_coalescing_and_partial_writes() {
    const std::size_t chunks[] = {1, 2, 7, 31, 256, 4096};
    for (std::size_t chunk : chunks) {
        Pair pair;
        Bytes joined;
        std::vector<Bytes> expected;
        for (unsigned i = 0; i < kWindow; ++i) {
            expected.emplace_back(kMaxPayload - i, static_cast<uint8_t>(i + 17));
            send(pair.a, expected.back());
            std::size_t size = 0;
            const uint8_t *p = pair.a.output(size);
            CHECK(size > 256);
            CHECK(!pair.a.consumeOutput(size + 1));
            joined.insert(joined.end(), p, p + 1);
            CHECK(pair.a.consumeOutput(1));
            p = pair.a.output(size);
            joined.insert(joined.end(), p, p + size);
            CHECK(pair.a.consumeOutput(size));
        }
        CHECK(joined.size() > 1024);
        CHECK(deliver(pair.b, joined, 1, chunk));
        Radio radio; radio.service(pair.b, 1);
        CHECK(radio.queued == expected);
        pump(pair.a, pair.b, 1, chunk);
        for (unsigned i = 0; i < kWindow; ++i) result(pair.a, Delivery::AcceptedIntoRadioQueue);
    }
}

static void malformed_and_truncated_frames() {
    const Bytes valid = encoded(dataFrame());
    const std::pair<std::size_t, uint8_t> corruptions[] = {
        {0, 'X'}, {1, 'X'}, {2, 2}, {3, 99}, {4, 0xff}, {6, 1}, {7, 1},
        {8, 3}, {32, 0}, {valid.size() - 1, 0}
    };
    for (const auto &corruption : corruptions) {
        Pair pair;
        Bytes bytes = valid; bytes[corruption.first] = corruption.second;
        CHECK(!deliver(pair.b, bytes, 1));
        CHECK(pair.b.reason() == Reason::Protocol);
        CHECK(pair.b.incomingCount() == 0);
        CHECK(!deliver(pair.b, valid, 2));
    }
    Frame f = dataFrame(); f.length = kMaxPayload;
    const Bytes full = encoded(f);
    for (std::size_t length = 1; length < full.size(); ++length) {
        Pair pair;
        CHECK(pair.b.input(full.data(), length, 0));
        CHECK(pair.b.incomingCount() == 0);
        pair.b.tick(kIdleMs);
        CHECK(pair.b.state() == State::Closed);
    }
    Pair pair;
    CHECK(!pair.b.input(nullptr, 1, 0));
}

static void admission_window_and_result_backpressure() {
    Pair pair;
    for (unsigned i = 0; i < kWindow; ++i) send(pair.a, Bytes(3, static_cast<uint8_t>(i)));
    uint32_t sequence = 999;
    const uint8_t byte = 1;
    CHECK(pair.a.submit(&byte, 1, 0, sequence) == Submit::Backpressure && sequence == 0);
    pump(pair.a, pair.b, 0);
    CHECK(pair.b.incomingCount() == kWindow && pair.a.pending() == kWindow);
    Result r; CHECK(!pair.a.takeResult(r)); // Bytes in TCP/parser aren't a radio admission.
    Radio radio; radio.capacity = 2; radio.service(pair.b);
    CHECK(radio.attempts == 4 && radio.queued.size() == 2);
    pump(pair.a, pair.b, 0);
    CHECK(pair.a.pending() == 0);
    CHECK(pair.a.submit(&byte, 1, 0, sequence) == Submit::Backpressure); // Results also bounded.
    for (unsigned i = 0; i < kWindow; ++i)
        CHECK(result(pair.a, i < 2 ? Delivery::AcceptedIntoRadioQueue : Delivery::Rejected).sequence == i + 1);
    CHECK(pair.a.submit(nullptr, 1, 0, sequence) == Submit::Invalid);
    CHECK(pair.a.submit(&byte, kMaxPayload + 1, 0, sequence) == Submit::Invalid);
    CHECK(pair.a.submit(&byte, 0, 0, sequence) == Submit::Invalid);
    CHECK(pair.a.statistics().peakPending == kWindow);
    CHECK(pair.b.statistics().peakIncoming == kWindow);
}

static void duplicate_pending_completed_and_evicted() {
    Pair pair;
    send(pair.a, Bytes{1, 2, 3});
    const Bytes original = next(pair.a);
    CHECK(deliver(pair.b, original, 0)); CHECK(deliver(pair.b, original, 0));
    CHECK(pair.b.incomingCount() == 1);
    CHECK(next(pair.b).empty());
    Radio radio; radio.service(pair.b);
    CHECK(deliver(pair.b, original, 0));
    pump(pair.a, pair.b, 0);
    result(pair.a, Delivery::AcceptedIntoRadioQueue);
    Result r; CHECK(!pair.a.takeResult(r));
    CHECK(pair.a.statistics().unexpectedConfirmations == 1);
    for (unsigned i = 0; i < 12; ++i) {
        radio.queued.clear();
        send(pair.a, Bytes{static_cast<uint8_t>(i)});
        pump(pair.a, pair.b, 0); radio.service(pair.b); pump(pair.a, pair.b, 0);
        result(pair.a, Delivery::AcceptedIntoRadioQueue);
    }
    CHECK(deliver(pair.b, original, 0));
    radio.service(pair.b);
    CHECK(radio.attempts == 13 && pair.b.statistics().duplicateData == 3);
}

static void reconnect_stale_epochs_and_no_replay() {
    Pair pair; Radio radio;
    send(pair.a, Bytes{1});
    const Bytes oldData = next(pair.a);
    CHECK(deliver(pair.b, oldData, 0)); radio.service(pair.b);
    const Bytes oldConfirm = next(pair.b); // Accepted, but acknowledgement lost.
    pair.a.close(); pair.b.close();
    CHECK(!pair.a.open(11, 1)); // Cannot overwrite an unconsumed result.
    CHECK(result(pair.a, Delivery::Unknown).reason == Reason::Disconnect);
    CHECK(!pair.a.open(10, 1)); CHECK(!pair.b.open(20, 1));
    CHECK(pair.a.open(11, 1)); CHECK(pair.b.open(21, 1)); pump(pair.a, pair.b, 1);
    CHECK(pair.b.incomingCount() == 0);
    CHECK(send(pair.a, Bytes{2}, 1) == 1);
    CHECK(deliver(pair.a, oldConfirm, 1)); CHECK(pair.a.pending() == 1);
    CHECK(deliver(pair.b, oldData, 1)); CHECK(pair.b.incomingCount() == 0);
    pump(pair.a, pair.b, 1); radio.service(pair.b, 1); pump(pair.a, pair.b, 1);
    result(pair.a, Delivery::AcceptedIntoRadioQueue);
    CHECK(radio.queued == std::vector<Bytes>({Bytes{1}, Bytes{2}}));
    CHECK(pair.a.statistics().staleFrames == 1 && pair.b.statistics().staleFrames == 1);
}

static void handshake_and_incompatible_peer() {
    Endpoint a(1, 2), wrong(3, 1);
    CHECK(a.open(1, 0)); CHECK(wrong.open(1, 0));
    CHECK(!deliver(a, next(wrong), 0)); CHECK(a.reason() == Reason::Protocol);
    Endpoint timeout(1, 2);
    CHECK(timeout.open(1, 0));
    uint32_t sequence; const uint8_t byte = 1;
    CHECK(timeout.submit(&byte, 1, 0, sequence) == Submit::NotReady);
    timeout.tick(kHandshakeMs - 1); CHECK(timeout.state() == State::Handshaking);
    timeout.tick(kHandshakeMs); CHECK(timeout.reason() == Reason::HandshakeTimeout);
    Endpoint self(1, 1), zero(0, 2);
    CHECK(!self.open(1, 0) && !zero.open(1, 0));
    Frame invalid = dataFrame(); invalid.type = Type::Hello; invalid.sequence = 0;
    invalid.targetEpoch = 0; invalid.length = 4;
    uint8_t buffer[kMaxWireSize];
    CHECK(!encode(invalid, buffer, sizeof(buffer))); // Incompatible capabilities.
    Pair pair;
    Endpoint restarted(2, 1); CHECK(restarted.open(21, 0));
    CHECK(!deliver(pair.a, next(restarted), 0));
    CHECK(pair.a.reason() == Reason::Protocol);
}

static void loss_timeout_and_late_confirmation() {
    for (bool admit : {false, true}) {
        Pair pair; Radio radio;
        send(pair.a, Bytes{7});
        const Bytes data = next(pair.a);
        Bytes confirmation;
        if (admit) {
            CHECK(deliver(pair.b, data, 1)); radio.service(pair.b, 1);
            confirmation = next(pair.b);
        } // Otherwise discard complete request (adapter fault, not TCP packet loss).
        pair.a.tick(kTransactionMs - 1); CHECK(pair.a.pending() == 1);
        if (admit) CHECK(!deliver(pair.a, confirmation, kTransactionMs));
        else pair.a.tick(kTransactionMs);
        CHECK(pair.a.reason() == Reason::TransactionTimeout);
        CHECK(result(pair.a, Delivery::Unknown).reason == Reason::TransactionTimeout);
        CHECK(next(pair.a).empty());
        CHECK(radio.attempts == (admit ? 1U : 0U));
    }
}

static void radio_stall_expires_without_admission() {
    Pair pair; Radio radio;
    send(pair.a, Bytes{9}); pump(pair.a, pair.b, 0);
    radio.service(pair.b, kTransactionMs);
    CHECK(radio.attempts == 0 && pair.b.incomingCount() == 0);
    const Frame rejected = decoded(next(pair.b));
    CHECK(rejected.type == Type::Confirm && rejected.payload[0] == 1);
    pair.a.tick(kTransactionMs);
    result(pair.a, Delivery::Unknown);
}

static void heartbeat_and_blackhole() {
    Pair pair;
    for (uint64_t now = 100; now <= 5000; now += 100) {
        pair.a.tick(now); pair.b.tick(now); pump(pair.a, pair.b, now);
        CHECK(pair.a.state() == State::Ready && pair.b.state() == State::Ready);
    }
    for (uint64_t now = 5100; now < 6000; now += 100) {
        pair.a.tick(now); pair.b.tick(now);
        next(pair.a); next(pair.b); // Blackhole both directions.
        CHECK(pair.a.state() == State::Ready && pair.b.state() == State::Ready);
    }
    pair.a.tick(6000); pair.b.tick(6000);
    CHECK(pair.a.reason() == Reason::IdleTimeout && pair.b.reason() == Reason::IdleTimeout);
}

static void control_priority_preserves_partial_frame() {
    Pair pair; Radio radio;
    send(pair.a, Bytes{1}); send(pair.a, Bytes{2}); send(pair.b, Bytes{3});
    std::size_t size; const uint8_t *p = pair.a.output(size);
    Bytes first(p, p + 3); CHECK(pair.a.consumeOutput(3));
    CHECK(deliver(pair.b, first, 0));
    CHECK(deliver(pair.a, next(pair.b), 0)); radio.service(pair.a);
    const Bytes rest = next(pair.a); CHECK(deliver(pair.b, rest, 0));
    first.insert(first.end(), rest.begin(), rest.end());
    CHECK(decoded(first).type == Type::Data);
    CHECK(decoded(next(pair.a)).type == Type::Confirm);
    CHECK(decoded(next(pair.a)).type == Type::Data);
}

static void bounded_input_and_control_overflow() {
    Pair pair; Radio radio;
    for (uint32_t seq = 1; seq <= 5; ++seq) CHECK(deliver(pair.b, encoded(dataFrame(seq)), 0));
    CHECK(pair.b.incomingCount() == 4);
    const Frame rejection = decoded(next(pair.b));
    CHECK(rejection.sequence == 5 && rejection.payload[0] == 1);
    radio.service(pair.b);
    while (!next(pair.b).empty()) {}
    const Bytes duplicate = encoded(dataFrame());
    for (unsigned i = 0; i < 16; ++i) CHECK(deliver(pair.b, duplicate, 0));
    CHECK(!deliver(pair.b, duplicate, 0));
    CHECK(pair.b.reason() == Reason::ControlOverflow && radio.attempts == 4);
}

static void reconnect_100_cycles() {
    Pair pair;
    for (uint64_t cycle = 1; cycle <= 100; ++cycle) {
        for (unsigned i = 0; i < kWindow; ++i) send(pair.a, Bytes{42}, cycle);
        // Close even with a partially written frame and a mixture of queued requests.
        std::size_t size; CHECK(pair.a.output(size)); CHECK(pair.a.consumeOutput(1));
        pair.a.close(); pair.b.close();
        for (unsigned i = 0; i < kWindow; ++i) result(pair.a, Delivery::Unknown);
        Result extra; CHECK(!pair.a.takeResult(extra));
        CHECK(pair.a.open(10 + cycle, cycle)); CHECK(pair.b.open(20 + cycle, cycle));
        pump(pair.a, pair.b, cycle);
        CHECK(pair.b.incomingCount() == 0 && pair.a.pending() == 0);
    }
}

static void randomized_stream_stress_20000_requests() {
    Pair pair; Radio ra, rb;
    std::mt19937 rng(0x4342435a);
    for (uint64_t batch = 0; batch < 2500; ++batch) {
        std::vector<Bytes> expectedA, expectedB;
        for (unsigned slot = 0; slot < kWindow; ++slot) {
            Bytes a(1 + rng() % kMaxPayload), b(1 + rng() % kMaxPayload);
            for (auto &byte : a) byte = static_cast<uint8_t>(rng());
            for (auto &byte : b) byte = static_cast<uint8_t>(rng());
            send(pair.a, a, batch); send(pair.b, b, batch);
            expectedA.push_back(b); expectedB.push_back(a);
        }
        pump(pair.a, pair.b, batch, 1 + rng() % 400);
        ra.service(pair.a, batch); rb.service(pair.b, batch);
        CHECK(ra.queued == expectedA && rb.queued == expectedB);
        pump(pair.a, pair.b, batch, 1 + rng() % 400);
        for (unsigned slot = 0; slot < kWindow; ++slot) {
            result(pair.a, Delivery::AcceptedIntoRadioQueue);
            result(pair.b, Delivery::AcceptedIntoRadioQueue);
        }
        ra.queued.clear(); rb.queued.clear();
    }
    CHECK(ra.attempts == 10000 && rb.attempts == 10000);
}

struct Transit { uint64_t due; Bytes bytes; };
static void delayed_link_1_5_20_100ms() {
    for (uint64_t delay : {1, 5, 20, 100}) {
        Endpoint a(1, 2), b(2, 1); Radio radio;
        CHECK(a.open(10, 0)); CHECK(b.open(20, 0));
        std::deque<Transit> ab, ba;
        unsigned sent = 0, confirmed = 0;
        uint64_t lastSubmit = 0;
        std::vector<uint64_t> rtt;
        for (uint64_t now = 0; now < 100000 && confirmed < 100; ++now) {
            a.tick(now); b.tick(now);
            CHECK(a.state() != State::Closed && b.state() != State::Closed);
            while (!ab.empty() && ab.front().due <= now) {
                CHECK(deliver(b, ab.front().bytes, now, 1)); ab.pop_front();
            }
            while (!ba.empty() && ba.front().due <= now) {
                CHECK(deliver(a, ba.front().bytes, now, 3)); ba.pop_front();
            }
            radio.service(b, now); radio.queued.clear();
            Result r;
            while (a.takeResult(r)) {
                CHECK(r.delivery == Delivery::AcceptedIntoRadioQueue);
                ++confirmed; rtt.push_back(now - lastSubmit);
            }
            if (a.state() == State::Ready && sent == confirmed && sent < 100) {
                send(a, Bytes(kMaxPayload, static_cast<uint8_t>(sent)), now);
                ++sent; lastSubmit = now;
            }
            for (Bytes bytes = next(a); !bytes.empty(); bytes = next(a))
                ab.push_back(Transit{now + delay, bytes});
            for (Bytes bytes = next(b); !bytes.empty(); bytes = next(b))
                ba.push_back(Transit{now + delay, bytes});
        }
        CHECK(confirmed == 100 && radio.attempts == 100);
        CHECK(*std::min_element(rtt.begin(), rtt.end()) == 2 * delay);
        CHECK(*std::max_element(rtt.begin(), rtt.end()) == 2 * delay);
        std::cout << "  virtual one-way=" << delay << "ms RTT=" << 2 * delay
                  << "ms accepted=" << confirmed << '\n';
    }
}

static void parser_random_noise_and_mutation_100000_cases() {
    std::mt19937 rng(0xdeadbeef);
    const Bytes baseline = encoded(dataFrame());
    for (unsigned trial = 0; trial < 100000; ++trial) {
        Bytes bytes;
        if (trial % 2) {
            bytes = baseline;
            bytes[rng() % bytes.size()] ^= static_cast<uint8_t>(1U << (rng() % 8));
        } else {
            bytes.resize(1 + rng() % 600);
            for (auto &byte : bytes) byte = static_cast<uint8_t>(rng());
        }
        Decoder decoder; Frame frame;
        for (uint8_t byte : bytes) {
            const Decoder::Step step = decoder.feed(byte, frame);
            CHECK(step != Decoder::Step::Complete);
            if (step == Decoder::Step::Error) {
                CHECK(decoder.feed(0, frame) == Decoder::Step::Error);
                break;
            }
        }
    }
}

int main() {
    const std::pair<const char *, void (*)()> tests[] = {
        {"golden_and_all_payload_lengths", golden_and_all_payload_lengths},
        {"fragmentation_coalescing_and_partial_writes", fragmentation_coalescing_and_partial_writes},
        {"malformed_and_truncated_frames", malformed_and_truncated_frames},
        {"admission_window_and_result_backpressure", admission_window_and_result_backpressure},
        {"duplicate_pending_completed_and_evicted", duplicate_pending_completed_and_evicted},
        {"reconnect_stale_epochs_and_no_replay", reconnect_stale_epochs_and_no_replay},
        {"handshake_and_incompatible_peer", handshake_and_incompatible_peer},
        {"loss_timeout_and_late_confirmation", loss_timeout_and_late_confirmation},
        {"radio_stall_expires_without_admission", radio_stall_expires_without_admission},
        {"heartbeat_and_blackhole", heartbeat_and_blackhole},
        {"control_priority_preserves_partial_frame", control_priority_preserves_partial_frame},
        {"bounded_input_and_control_overflow", bounded_input_and_control_overflow},
        {"reconnect_100_cycles", reconnect_100_cycles},
        {"randomized_stream_stress_20000_requests", randomized_stream_stress_20000_requests},
        {"delayed_link_1_5_20_100ms", delayed_link_1_5_20_100ms},
        {"parser_random_noise_and_mutation_100000_cases", parser_random_noise_and_mutation_100000_cases}
    };
    unsigned passed = 0;
    try {
        for (const auto &test : tests) {
            test.second(); ++passed; std::cout << "PASS " << test.first << std::endl;
        }
        std::cout << "RESULT " << passed << "/" << sizeof(tests) / sizeof(tests[0])
                  << " passed; sizeof(Endpoint)=" << sizeof(Endpoint) << " bytes\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL after " << passed << " tests: " << error.what() << '\n';
        return 1;
    }
}
