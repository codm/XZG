#ifndef CZC_BACKHAUL_H
#define CZC_BACKHAUL_H

#include <array>
#include <cstddef>
#include <cstdint>

// Portable framed transport. No Arduino, sockets, Zigbee, or heap allocation.
namespace backhaul {
constexpr std::size_t kMaxPayload = 256;
constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kMaxWireSize = kHeaderSize + kMaxPayload + 4;
constexpr std::size_t kWindow = 4;
constexpr uint64_t kHandshakeMs = 500;
constexpr uint64_t kTransactionMs = 500;
constexpr uint64_t kHeartbeatMs = 100;
constexpr uint64_t kIdleMs = 1000;

enum class Type : uint8_t { Hello = 1, HelloAck, Data, Confirm, Heartbeat };
enum class State { Closed, Handshaking, Ready };
enum class Reason { None, Disconnect, Protocol, HandshakeTimeout, IdleTimeout,
                    TransactionTimeout, ControlOverflow, SequenceExhausted };
enum class Delivery { AcceptedIntoRadioQueue, Rejected, Unknown };
enum class Submit { Queued, NotReady, Backpressure, Invalid };

struct Frame {
    Type type = Type::Hello;
    uint32_t node = 0;
    uint64_t epoch = 0;
    uint64_t targetEpoch = 0;
    uint32_t sequence = 0;
    uint16_t length = 0;
    std::array<uint8_t, kMaxPayload> payload{};
};

struct Result {
    uint32_t sequence = 0;
    Delivery delivery = Delivery::Unknown;
    Reason reason = Reason::None;
};

template <typename T, std::size_t N> class Queue {
public:
    bool push(const T &value) {
        if (size_ == N) return false;
        items_[(head_ + size_) % N] = value;
        ++size_;
        return true;
    }
    const T *front() const { return size_ ? &items_[head_] : nullptr; }
    bool pop(T &value) {
        if (!size_) return false;
        value = items_[head_];
        items_[head_] = T{};
        head_ = (head_ + 1) % N;
        --size_;
        return true;
    }
    void clear() { items_.fill(T{}); head_ = size_ = 0; }
    std::size_t size() const { return size_; }
private:
    std::array<T, N> items_{};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};

// Zero means invalid frame/capacity. All integer fields use network byte order.
std::size_t encode(const Frame &frame, uint8_t *output, std::size_t capacity);

class Decoder {
public:
    enum class Step { More, Complete, Error };
    Step feed(uint8_t byte, Frame &frame);
    void reset();
private:
    std::array<uint8_t, kMaxWireSize> bytes_{};
    std::size_t size_ = 0;
    std::size_t expected_ = 0;
    bool failed_ = false;
};

struct Statistics {
    uint64_t duplicateData = 0;
    uint64_t staleFrames = 0;
    uint64_t unexpectedConfirmations = 0;
    std::size_t peakIncoming = 0;
    std::size_t peakPending = 0;
};

class Endpoint {
public:
    Endpoint(uint32_t node, uint32_t expectedPeer);
    // Call for a NEW byte stream. Epoch must increase, including across reboot
    // (persistence/provisioning belongs to the future firmware integration).
    // Drain results before opening another session.
    bool open(uint64_t epoch, uint64_t now);
    void close(Reason reason = Reason::Disconnect);
    void tick(uint64_t now);
    bool input(const uint8_t *bytes, std::size_t length, uint64_t now);
    // Keep sending the returned frame until consumed. Control cannot interleave
    // with a partially written data frame. Consume only bytes actually written.
    const uint8_t *output(std::size_t &length);
    bool consumeOutput(std::size_t length);
    Submit submit(const uint8_t *bytes, std::size_t length, uint64_t now,
                  uint32_t &sequence);
    // Synchronous, non-reentrant adapter callback returns true only after its
    // bounded radio queue accepts the frame. Expired input never reaches it.
    using Admission = bool (*)(const Frame &, void *);
    bool serviceIncoming(Admission admit, void *context, uint64_t now);
    bool takeResult(Result &result) { return results_.pop(result); }
    State state() const { return state_; }
    Reason reason() const { return reason_; }
    std::size_t pending() const;
    std::size_t incomingCount() const { return incoming_.size(); }
    const Statistics &statistics() const { return statistics_; }

private:
    struct Pending { uint32_t sequence = 0; uint64_t since = 0; };
    struct Incoming { Frame frame; uint64_t since = 0; };
    struct History { uint32_t sequence = 0; bool accepted = false; };
    Frame makeFrame(Type type) const;
    bool control(const Frame &frame);
    void receive(const Frame &frame, uint64_t now);
    void confirm(uint32_t sequence, bool accepted);
    void remember(uint32_t sequence, bool accepted);
    bool resolveIncoming(bool accepted);

    uint32_t node_;
    uint32_t expectedPeer_;
    State state_ = State::Closed;
    Reason reason_ = Reason::None;
    uint64_t epoch_ = 0;
    uint64_t peerEpoch_ = 0;
    uint64_t openedAt_ = 0;
    uint64_t lastRx_ = 0;
    uint64_t lastHeartbeat_ = 0;
    uint32_t nextSequence_ = 1;
    uint32_t highestReceived_ = 0;
    bool helloAckReceived_ = false;
    Decoder decoder_;
    struct Control { std::array<uint8_t,kHeaderSize+4+4> wire{}; std::size_t length=0; };
    Queue<Control, 16> controls_;
    Queue<Frame, kWindow> data_;
    Queue<Incoming, kWindow> incoming_;
    Queue<Result, kWindow> results_;
    std::array<Pending, kWindow> pending_{};
    std::array<History, 8> history_{};
    std::size_t historyNext_ = 0;
    std::array<uint8_t, kMaxWireSize> wire_{};
    std::size_t wireSize_ = 0;
    std::size_t wireOffset_ = 0;
    Statistics statistics_;
};
} // namespace backhaul
#endif
