#ifndef MPP_HPP
#define MPP_HPP

#include "enet.h"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>

namespace mpp {

template<typename T>
class EventHandler {
public:
    std::vector<T> callbacks;

    template<typename ... Param>
    void operator () (Param ... param) {
        for(size_t i = 0; i < callbacks.size(); i++) {
            callbacks[i](param...);
        }
    }

    void operator += (T callback) {
        callbacks.push_back(callback);
    }
};

template <typename T>
class RingBuffer {
public:
    RingBuffer() {
        size_t capacity = 1024;
        capacity = nextPowerOfTwo(capacity);
        _modMask = capacity - 1;
        _entries.resize(capacity);
    }

    RingBuffer(size_t capacity) {
        capacity = nextPowerOfTwo(capacity);
        _modMask = capacity - 1;
        _entries.resize(capacity);
    }

    // Add an item to the end of the container
    void enqueue(T &item) {
        auto next = _producerCursor.fetch_add(1, std::memory_order_acq_rel) + 1;

        // Wait for space to be available
        long wrapPoint = next - _entries.size();
        long min = _consumerCursor.load(std::memory_order_acquire);
        while (wrapPoint > min) {
            min = _consumerCursor.load(std::memory_order_acquire);
            std::this_thread::yield();
        }

        // Add item to the container
        _entries[next & _modMask] = item;

        // Update producer cursor
        _producerCursor.store(next, std::memory_order_release);
    }

    // Remove an item from the beginning of the container
    T dequeue() {
        auto next = _consumerCursor.fetch_add(1, std::memory_order_acq_rel) + 1;

        // Wait for data to be available
        while (_producerCursor.load(std::memory_order_acquire) < next) {
            std::this_thread::yield();
        }

        // Get item from the container
        auto result = _entries[next & _modMask];

        // Update consumer cursor
        _consumerCursor.store(next, std::memory_order_release);

        return result;
    }

    // Try to remove an item from the beginning of the container
    bool tryDequeue(T &obj) {
        auto next = _consumerCursor.load(std::memory_order_acquire) + 1;

        // Check if data is available
        if (_producerCursor.load(std::memory_order_acquire) < next) {
            return false;
        }

        // Get item from the container
        obj = dequeue();

        return true;
    }

    // Get the number of items in the container
    int count() const {
        return static_cast<int>(_producerCursor.load(std::memory_order_acquire) -
                                _consumerCursor.load(std::memory_order_acquire));
    }

    void drain() {
        T val;
        while (tryDequeue(val)) {
        }
    }

private:
    std::vector<T> _entries;
    size_t _modMask;
    std::atomic_size_t _consumerCursor = 0;
    std::atomic_size_t _producerCursor = 0;

    // Compute the next power of two
    static size_t nextPowerOfTwo(size_t n) {
        size_t result = 2;
        while (result < n) {
            result <<= 1;
        }
        return result;
    }
};

typedef unsigned char byte;

enum NetworkPacketFlags {
    PacketFlags_None = 0,
    PacketFlags_Reliable = 1 << 0,
    PacketFlags_Unsequenced = 1 << 1,
    PacketFlags_NoAllocate = 1 << 2,
    PacketFlags_UnreliableFragmented = 1 << 3,
    PacketFlags_Instant = 1 << 4,
    PacketFlags_Unthrottled = 1 << 5,
    PacketFlags_Sent =  1 << 8
};

enum NetworkChannelTypes {
    Reliable = PacketFlags_Reliable,                                        // Reliable UDP (TCP-like emulation)
    ReliableUnsequenced = PacketFlags_Reliable | PacketFlags_Unsequenced,   // Reliable UDP (TCP-like emulation w/o sequencing)
    Unreliable = PacketFlags_Unsequenced,                                   // Pure UDP, high velocity packet action.
    UnreliableFragmented = PacketFlags_UnreliableFragmented,                // Pure UDP, but fragmented.
    UnreliableSequenced = PacketFlags_None,                                 // Pure UDP, but sequenced.
    Unthrottled = PacketFlags_Unthrottled                                  // Pure UDP. Literally turbo mode.
};

struct NetworkInternals {
    std::string version = "1.4.0r2 (LTS)";
    std::string scheme = "enet";
    std::string bindAnyAddress = "::0";
};

enum NetworkLogType {
    LogType_Quiet,
    LogType_Standard,
    LogType_Verbose
};

struct NetworkIncomingPacket {
    byte channel;
    uint32_t peerId;
    ENetPacket *payload;
};

struct NetworkOutgoingPacket {
    byte channel;
    uint16_t peerId;
    std::vector<uint16_t> peers;
    int numPeers;
    ENetPacket *payload;
};

struct NetworkConnectionEvent {
    byte eventType;
    uint16_t port;
    uint16_t peerId;
    std::string IP;
};

enum NetworkCommandType {
    // Client
    NetworkCommandType_ClientWantsToStop,
    NetworkCommandType_ClientStatusRequest,
    // Server
    NetworkCommandType_ServerKickPeer,
    NetworkCommandType_ServerStatusRequest
};

struct NetworkCommandPacket {
    NetworkCommandType type;
    uint16_t peerId;
};

// Stats only - may not always be used!
struct NetworkClientStats {
    uint32_t RTT;
    uint64_t bytesReceived;
    uint64_t bytesSent;
    uint64_t packetsReceived;
    uint64_t packetsSent;
    uint64_t packetsLost;
};

// Stats only - may not always be used!
struct NetworkServerStats {
    uint64_t bytesReceived;
    uint64_t bytesSent;
    uint64_t packetsReceived;
    uint64_t packetsSent;
    uint64_t peersCount;
    std::unordered_map<int, NetworkClientStats> peerStats;
};

struct NetworkPeerConnectionData {        
    uint16_t port;
    uint16_t peerId;
    std::string IP;
};

struct ServerConfiguration {
    bool isFruityDevice;
    bool bindAllInterfaces;
    std::string address;
    uint32_t port;
    uint32_t peers;
    uint32_t channels;
    uint32_t pollTime;
    uint32_t packetSizeLimit;
    uint32_t verbosity;
};

}

namespace mpp::servers {

typedef std::function<void(NetworkIncomingPacket &incoming)> ReceivedEvent;
typedef std::function<void(NetworkConnectionEvent &connection)> ConnectedEvent;
typedef std::function<void(NetworkConnectionEvent &connection)> DisconnectedEvent;
typedef std::function<void(NetworkServerStats &stats)> StatsEvent;

class Server {
public:
    EventHandler<ReceivedEvent> received;
    EventHandler<ConnectedEvent> connected;
    EventHandler<DisconnectedEvent> disconnected;
    EventHandler<StatsEvent> statsUpdate;
    Server(uint16_t port, uint16_t maxClients);
    ~Server();
    void start();
    void update();
    void stop();
    bool isRunning() const;
    void send(uint16_t peerId, byte *data, size_t size, byte channel, NetworkPacketFlags flags);
    void broadcast(byte *data, size_t size, byte channel, NetworkPacketFlags flags);
    void broadcastSelective(const std::vector<uint16_t> &peerIds, byte *data, size_t size, byte channel, NetworkPacketFlags flags);
    void kick(uint16_t peerId);
private:
    ENetAddress serverAddress;
    ENetHost *server;
    std::thread networkThread;
    std::atomic<bool> runFlag;      
    std::vector<ENetPeer*> serverPeerArray;
    std::string bindAddress = "0.0.0.0";
    uint16_t bindPort = 7777;        
    uint32_t maximumChannels = 2;
    uint32_t maximumPeers = 100;
    uint32_t maximumPacketSize = 33554432;    // ENet.cs: uint maxPacketSize = 32 * 1024 * 1024 = 33554432        
    uint32_t pollTime = 15;        
    uint32_t verbosity = 1;        
    uint32_t incomingOutgoingBufferSize = 4096;
    uint32_t connectionEventBufferSize = 128;
    bool isFruityDevice = false;
    bool bindAllInterfaces = false;  
    std::unique_ptr<RingBuffer<NetworkIncomingPacket>> incoming;
    std::unique_ptr<RingBuffer<NetworkOutgoingPacket>> outgoing;
    std::unique_ptr<RingBuffer<NetworkCommandPacket>> commands;
    std::unique_ptr<RingBuffer<NetworkConnectionEvent>> connectionEvents;
    std::unique_ptr<RingBuffer<NetworkConnectionEvent>> disconnectionEvents;
    std::unique_ptr<RingBuffer<NetworkServerStats>> statusUpdates;
    std::unique_ptr<RingBuffer<NetworkServerStats>> recycledServerStatBlocks;
    void setupRingBuffersIfNull();
    void drainQueues();
    void networkUpdate(const ServerConfiguration &setupInfo);
    void handleCommands(const ServerConfiguration &setupInfo);
    void handleOutgoingPackets(const ServerConfiguration &setupInfo);
    void handleNetworkEvents(const ServerConfiguration &setupInfo);
};

}

namespace mpp::clients {

struct ConnectionInfo {
    ConnectionInfo() {}
};

struct NetworkConfig {
    uint16_t port;
    std::string hostAddress;
    uint32_t maxChannels;
    uint32_t maxPacketSize;
    uint32_t bufferSize;
    uint32_t incomingQueueCapacity;
    uint32_t outgoingQueueCapacity;

    NetworkConfig(uint16_t port, const std::string &hostAddress, uint32_t maxChannels, uint32_t bufferSize, uint32_t maxPacketSize, uint32_t incomingQueueCapacity, uint32_t outgoingQueueCapacity) {
        this->port = port;
        this->hostAddress = hostAddress;
        this->maxChannels = maxChannels;
        this->bufferSize = bufferSize;
        this->maxPacketSize = maxPacketSize;
        this->incomingQueueCapacity = incomingQueueCapacity;
        this->outgoingQueueCapacity = outgoingQueueCapacity;
    }
    
    NetworkConfig() {}
};

typedef std::function<void()> ConnectedEvent;
typedef std::function<void()> DisconnectedEvent;
typedef std::function<void(NetworkIncomingPacket &incoming)> ReceivedEvent;

class Client {
public:
    EventHandler<ConnectedEvent> connected;
    EventHandler<DisconnectedEvent> disconnected;
    EventHandler<ReceivedEvent> received;
    Client(const NetworkConfig &config);
    bool isRunning() const;
    void start();
    void stop();
    void update();
    void send(byte *data, size_t length, byte channelId, ENetPacketFlag flags);
private:
    NetworkConfig config;
    std::atomic<bool> runFlag;
    std::thread networkThread;
    std::vector<unsigned char> incomingBuffer;
    std::vector<unsigned char> outgoingBuffer;
    std::unique_ptr<RingBuffer<ConnectionInfo>> connectionQueue;
    std::unique_ptr<RingBuffer<ConnectionInfo>> disconnectionQueue;
    std::unique_ptr<RingBuffer<NetworkIncomingPacket>> incomingPacketQueue;
    std::unique_ptr<RingBuffer<NetworkOutgoingPacket>> outgoingPacketQueue;
    void networkUpdate();
};

}

#endif