#include "Backhaul.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace backhaul {
namespace {
void put(uint8_t *p, uint64_t value, std::size_t count) {
    while (count) { p[--count] = static_cast<uint8_t>(value); value >>= 8; }
}
uint64_t get(const uint8_t *p, std::size_t count) {
    uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) value = (value << 8) | p[i];
    return value;
}
uint32_t crc32(const uint8_t *p, std::size_t count) {
    uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < count; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0U);
    }
    return ~crc;
}
bool valid(const Frame &f) {
    if (!f.node || !f.epoch || f.length > kMaxPayload) return false;
    if (f.type == Type::Hello)
        return !f.targetEpoch && !f.sequence && f.length == 4 &&
               get(f.payload.data(), 2) == kWindow &&
               get(f.payload.data() + 2, 2) == kMaxPayload;
    if (!f.targetEpoch) return false;
    switch (f.type) {
    case Type::HelloAck:
    case Type::Heartbeat: return !f.sequence && !f.length;
    case Type::Data: return f.sequence && f.length;
    case Type::Confirm: return f.sequence && f.length == 1 && f.payload[0] <= 1;
    default: return false;
    }
}
} // namespace

std::size_t encode(const Frame &f, uint8_t *out, std::size_t capacity) {
    const std::size_t total = kHeaderSize + f.length + 4;
    if (!valid(f) || !out || capacity < total) return 0;
    out[0] = 'C'; out[1] = 'B'; out[2] = 1;
    out[3] = static_cast<uint8_t>(f.type);
    put(out + 4, f.length, 2);
    out[6] = out[7] = 0;
    put(out + 8, f.node, 4);
    put(out + 12, f.epoch, 8);
    put(out + 20, f.targetEpoch, 8);
    put(out + 28, f.sequence, 4);
    std::copy_n(f.payload.begin(), f.length, out + kHeaderSize);
    put(out + total - 4, crc32(out, total - 4), 4);
    return total;
}

void Decoder::reset() { bytes_.fill(0); size_ = expected_ = 0; failed_ = false; }
Decoder::Step Decoder::feed(uint8_t byte, Frame &frame) {
    if (failed_) return Step::Error;
    bytes_[size_++] = byte;
    if ((size_ == 1 && bytes_[0] != 'C') ||
        (size_ == 2 && bytes_[1] != 'B') ||
        (size_ == 3 && bytes_[2] != 1) ||
        (size_ == 4 && (bytes_[3] < 1 || bytes_[3] > 5)) ||
        (size_ == 6 && get(bytes_.data() + 4, 2) > kMaxPayload) ||
        (size_ == 8 && (bytes_[6] || bytes_[7]))) {
        failed_ = true;
        return Step::Error;
    }
    if (size_ == 6) expected_ = kHeaderSize + get(bytes_.data() + 4, 2) + 4;
    if (!expected_ || size_ < expected_) return Step::More;
    const uint8_t *p = bytes_.data();
    if (get(p + size_ - 4, 4) != crc32(p, size_ - 4)) {
        failed_ = true;
        return Step::Error;
    }
    frame = Frame{};
    frame.type = static_cast<Type>(p[3]);
    frame.length = static_cast<uint16_t>(get(p + 4, 2));
    frame.node = static_cast<uint32_t>(get(p + 8, 4));
    frame.epoch = get(p + 12, 8);
    frame.targetEpoch = get(p + 20, 8);
    frame.sequence = static_cast<uint32_t>(get(p + 28, 4));
    std::copy_n(p + kHeaderSize, frame.length, frame.payload.begin());
    if (!valid(frame)) { failed_ = true; return Step::Error; }
    reset();
    return Step::Complete;
}

Endpoint::Endpoint(uint32_t node, uint32_t expectedPeer)
    : node_(node), expectedPeer_(expectedPeer) {}

bool Endpoint::open(uint64_t epoch, uint64_t now) {
    if (state_ != State::Closed || results_.size() || !node_ || !expectedPeer_ ||
        node_ == expectedPeer_ || epoch <= epoch_) return false;
    epoch_ = epoch;
    peerEpoch_ = 0;
    openedAt_ = lastRx_ = lastHeartbeat_ = now;
    nextSequence_ = 1;
    highestReceived_ = 0;
    helloAckReceived_ = false;
    decoder_.reset();
    controls_.clear(); data_.clear(); incoming_.clear();
    pending_.fill(Pending{}); history_.fill(History{}); historyNext_ = 0;
    wire_.fill(0); decoder_.reset();
    wireSize_ = wireOffset_ = 0;
    state_ = State::Handshaking;
    reason_ = Reason::None;
    Frame hello = makeFrame(Type::Hello);
    hello.length = 4;
    put(hello.payload.data(), kWindow, 2);
    put(hello.payload.data() + 2, kMaxPayload, 2);
    return control(hello);
}

void Endpoint::close(Reason reason) {
    if (state_ == State::Closed) return;
    state_ = State::Closed;
    reason_ = reason;
    for (auto &p : pending_) {
        if (p.sequence) {
            Result r;
            r.sequence = p.sequence; r.reason = reason; r.delivery = Delivery::Unknown;
            results_.push(r); // Reserved by submit(): pending + results <= window.
            p = Pending{};
        }
    }
    controls_.clear(); data_.clear(); incoming_.clear();
    wire_.fill(0); decoder_.reset();
    wireSize_ = wireOffset_ = 0;
}

Frame Endpoint::makeFrame(Type type) const {
    Frame f;
    f.type = type; f.node = node_; f.epoch = epoch_; f.targetEpoch = peerEpoch_;
    return f;
}
bool Endpoint::control(const Frame &frame) {
    Control small;
    small.length=encode(frame,small.wire.data(),small.wire.size());
    if (small.length && controls_.push(small)) return true;
    close(Reason::ControlOverflow);
    return false;
}
std::size_t Endpoint::pending() const {
    std::size_t count = 0;
    for (const auto &p : pending_) if (p.sequence) ++count;
    return count;
}
Submit Endpoint::submit(const uint8_t *bytes, std::size_t length, uint64_t now,
                        uint32_t &sequence) {
    sequence = 0;
    tick(now);
    if (state_ != State::Ready) return Submit::NotReady;
    if (!bytes || !length || length > kMaxPayload) return Submit::Invalid;
    if (pending() + results_.size() >= kWindow) return Submit::Backpressure;
    if (nextSequence_ == std::numeric_limits<uint32_t>::max()) {
        close(Reason::SequenceExhausted);
        return Submit::NotReady;
    }
    Frame f = makeFrame(Type::Data);
    f.sequence = nextSequence_;
    f.length = static_cast<uint16_t>(length);
    std::copy_n(bytes, length, f.payload.begin());
    if (!data_.push(f)) return Submit::Backpressure;
    for (auto &p : pending_) if (!p.sequence) {
        p.sequence = nextSequence_; p.since = now; break;
    }
    sequence = nextSequence_++;
    statistics_.peakPending = std::max(statistics_.peakPending, pending());
    return Submit::Queued;
}

bool Endpoint::input(const uint8_t *bytes, std::size_t length, uint64_t now) {
    tick(now);
    if (state_ == State::Closed) return false;
    if (!bytes && length) { close(Reason::Protocol); return false; }
    for (std::size_t i = 0; i < length; ++i) {
        Frame f;
        const Decoder::Step step = decoder_.feed(bytes[i], f);
        if (step == Decoder::Step::Error) { close(Reason::Protocol); return false; }
        if (step == Decoder::Step::Complete) receive(f, now);
        if (state_ == State::Closed) return false;
    }
    return true;
}

const uint8_t *Endpoint::output(std::size_t &length) {
    length = 0;
    if (state_ == State::Closed) return nullptr;
    if (wireOffset_ == wireSize_) {
        Frame f; Control small;
        if (controls_.pop(small)) { wireSize_=small.length; std::copy_n(small.wire.begin(),wireSize_,wire_.begin()); }
        else {
            if (!data_.pop(f)) return nullptr;
            wireSize_ = encode(f, wire_.data(), wire_.size());
        }
        wireOffset_ = 0;
        if (!wireSize_) { close(Reason::Protocol); return nullptr; }
    }
    length = wireSize_ - wireOffset_;
    return wire_.data() + wireOffset_;
}
bool Endpoint::consumeOutput(std::size_t length) {
    if (length > wireSize_ - wireOffset_) return false;
    wireOffset_ += length;
    if(wireOffset_==wireSize_) wire_.fill(0);
    return true;
}

void Endpoint::remember(uint32_t sequence, bool accepted) {
    history_[historyNext_].sequence = sequence;
    history_[historyNext_].accepted = accepted;
    historyNext_ = (historyNext_ + 1) % history_.size();
}
void Endpoint::confirm(uint32_t sequence, bool accepted) {
    Frame f = makeFrame(Type::Confirm);
    f.sequence = sequence; f.length = 1; f.payload[0] = accepted ? 0 : 1;
    control(f);
}
bool Endpoint::serviceIncoming(Admission admit, void *context, uint64_t now) {
    tick(now);
    if (state_ != State::Ready || !incoming_.front() || !admit) return false;
    const bool accepted = admit(incoming_.front()->frame, context);
    return resolveIncoming(accepted);
}
bool Endpoint::resolveIncoming(bool accepted) {
    Incoming item;
    if (state_ != State::Ready || !incoming_.pop(item)) return false;
    remember(item.frame.sequence, accepted);
    confirm(item.frame.sequence, accepted);
    return true;
}

void Endpoint::receive(const Frame &f, uint64_t now) {
    if (f.node != expectedPeer_) { close(Reason::Protocol); return; }
    if (f.type == Type::Hello) {
        if (peerEpoch_ && peerEpoch_ != f.epoch) { close(Reason::Protocol); return; }
        peerEpoch_ = f.epoch;
        control(makeFrame(Type::HelloAck));
        lastRx_ = now;
        return;
    }
    if (!peerEpoch_ || f.epoch != peerEpoch_ || f.targetEpoch != epoch_) {
        ++statistics_.staleFrames;
        return;
    }
    if (f.type == Type::HelloAck) {
        helloAckReceived_ = true;
        state_ = State::Ready;
        lastRx_ = now;
        return;
    }
    if (!helloAckReceived_) { close(Reason::Protocol); return; }
    lastRx_ = now;
    if (f.type == Type::Data) {
        if (f.sequence <= highestReceived_) {
            ++statistics_.duplicateData;
            for (const auto &h : history_) if (h.sequence == f.sequence) {
                confirm(f.sequence, h.accepted);
                break;
            }
            return; // Pending/evicted sequences are never admitted twice.
        }
        highestReceived_ = f.sequence;
        Incoming item; item.frame = f; item.since = now;
        if (!incoming_.push(item)) {
            remember(f.sequence, false);
            confirm(f.sequence, false);
        }
        statistics_.peakIncoming = std::max(statistics_.peakIncoming, incoming_.size());
    } else if (f.type == Type::Confirm) {
        for (auto &p : pending_) if (p.sequence == f.sequence) {
            Result result;
            result.sequence = f.sequence;
            result.delivery = f.payload[0] == 0 ? Delivery::AcceptedIntoRadioQueue : Delivery::Rejected;
            results_.push(result);
            p = Pending{};
            return;
        }
        ++statistics_.unexpectedConfirmations;
    }
}

void Endpoint::tick(uint64_t now) {
    if (state_ == State::Closed) return;
    if (state_ == State::Handshaking && now - openedAt_ >= kHandshakeMs) {
        close(Reason::HandshakeTimeout); return;
    }
    if (now - lastRx_ >= kIdleMs) { close(Reason::IdleTimeout); return; }
    for (const auto &p : pending_) if (p.sequence && now - p.since >= kTransactionMs) {
        close(Reason::TransactionTimeout); return;
    }
    while (incoming_.front() && now - incoming_.front()->since >= kTransactionMs)
        if (!resolveIncoming(false)) break;
    if (state_ == State::Ready && now - lastHeartbeat_ >= kHeartbeatMs) {
        control(makeFrame(Type::Heartbeat));
        lastHeartbeat_ = now;
    }
}
} // namespace backhaul
