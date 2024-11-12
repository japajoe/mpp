#include "mpp.h"
#include <iostream>

namespace mpp {

static void writeLog(const std::string &message) {
    std::cout << message << std::endl;
}

}

namespace mpp::servers {

Server::Server(uint16_t port, uint16_t maxClients) {
    this->bindPort = port;
    this->maximumPeers = maxClients;
    this->runFlag = false;
    this->bindAllInterfaces = false;
}

Server::~Server() {
    stop();
}

void Server::start() {
    if (networkThread.joinable() || runFlag) {
        writeLog("A worker thread is already running");
        return;
    }

    runFlag = true;

    setupRingBuffersIfNull();
    drainQueues();

    ServerConfiguration info {
        .isFruityDevice = isFruityDevice,
        .bindAllInterfaces = bindAllInterfaces,
        .address = bindAddress,
        .port = bindPort,
        .peers = maximumPeers,
        .channels = maximumChannels,
        .pollTime = pollTime,
        .packetSizeLimit = maximumPacketSize,
        .verbosity = verbosity
    };

    networkThread = std::thread([this](const ServerConfiguration &paramInfo) { this->networkUpdate(paramInfo); }, info);
}

void Server::networkUpdate(const ServerConfiguration &setupInfo) {
    if (enet_initialize() != 0) {
        writeLog("Failed to initialize ENet");
        runFlag = false;
        return;
    }

    if (setupInfo.isFruityDevice) {
        if (!setupInfo.bindAllInterfaces)
            enet_address_set_hostname(&serverAddress, setupInfo.address.c_str());
    } else {
        if (setupInfo.bindAllInterfaces) {
            NetworkInternals internals;
            enet_address_set_ip(&serverAddress, internals.bindAnyAddress.c_str());
        } else {
            enet_address_set_hostname(&serverAddress, setupInfo.address.c_str());
        }
    }

    serverAddress.port = setupInfo.port;
    serverPeerArray.resize(setupInfo.peers);
    
    for(size_t i = 0; i < serverPeerArray.size(); i++)
        serverPeerArray[i] = nullptr;

    server = enet_host_create(&serverAddress, setupInfo.peers, setupInfo.channels, 0, 0, 0);

    if(server == nullptr) {
        writeLog("Couldn't create host. Please ensure you don't have a server already running on the same IP and Port");
        enet_deinitialize();
        runFlag = false;
        return;
    }

    writeLog("Server started listening on port " + std::to_string(bindPort));

    enet_host_set_checksum_callback(server, enet_crc64);

    while (runFlag) {
        handleCommands(setupInfo);
        handleOutgoingPackets(setupInfo);
        handleNetworkEvents(setupInfo);
    }

    if (verbosity > 0)
        writeLog("Server shutting down");

    enet_host_flush(server);

    for (size_t i = 0; i < serverPeerArray.size(); i++) {
        if (serverPeerArray[i] == nullptr) 
            continue;
        enet_peer_disconnect_now(serverPeerArray[i], 0);
    }

    if (setupInfo.verbosity > 0)
        writeLog("Server shutdown complete");

    enet_deinitialize();
}

void Server::handleCommands(const ServerConfiguration &setupInfo)
{
    NetworkClientStats peerStats;
    NetworkCommandPacket commandPacket;
    while(commands->tryDequeue(commandPacket)) {
        switch (commandPacket.type) {
            // Boot a Peer off the Server.
            case NetworkCommandType_ServerKickPeer: {
                uint16_t targetPeer = commandPacket.peerId;

                if (serverPeerArray[targetPeer] == nullptr) 
                    continue;

                NetworkConnectionEvent iced {
                    .eventType = 0x01,
                    .peerId = targetPeer
                };

                disconnectionEvents->enqueue(iced);

                // Disconnect and reset the peer array's entry for that peer.
                enet_peer_disconnect_now(serverPeerArray[targetPeer], 0);
                serverPeerArray[targetPeer] = nullptr;
                break;
            }
            case NetworkCommandType_ServerStatusRequest: {
                NetworkServerStats serverStats;
                if (!recycledServerStatBlocks->tryDequeue(serverStats))
                    serverStats.peerStats.clear();

                serverStats.peerStats.clear();
                serverStats.bytesReceived = enet_host_get_bytes_received(server);
                serverStats.bytesSent = enet_host_get_bytes_sent(server);
                serverStats.packetsReceived = enet_host_get_packets_received(server);
                serverStats.packetsSent = enet_host_get_packets_sent(server);
                serverStats.peersCount = enet_host_get_peers_count(server);

                for (size_t i = 0; i < serverPeerArray.size(); i++) {
                    if (serverPeerArray[i] == nullptr)
                        continue;

                    peerStats.RTT = enet_peer_get_rtt(serverPeerArray[i]);
                    peerStats.bytesReceived = enet_peer_get_bytes_received(serverPeerArray[i]);
                    peerStats.bytesSent = enet_peer_get_bytes_sent(serverPeerArray[i]);
                    peerStats.packetsSent = enet_peer_get_packets_sent(serverPeerArray[i]);
                    peerStats.packetsLost = enet_peer_get_packets_lost(serverPeerArray[i]);
                    serverStats.peerStats[i] = peerStats;
                }

                statusUpdates->enqueue(serverStats);
                break;
            }
            default: {
                break;
            }                
        }
    }
}

void Server::handleOutgoingPackets(const ServerConfiguration &setupInfo)
{
    NetworkOutgoingPacket outgoingPacket;

    while (outgoing->tryDequeue(outgoingPacket)) {
        if(outgoingPacket.peers.size() == 0) {
            if(outgoingPacket.numPeers == 1) {
                if(serverPeerArray[outgoingPacket.peerId] != nullptr)
                    enet_peer_send(serverPeerArray[outgoingPacket.peerId], outgoingPacket.channel, outgoingPacket.payload);
                else
                    enet_packet_dispose(outgoingPacket.payload);
            } else {
                enet_host_broadcast(server, outgoingPacket.channel, outgoingPacket.payload);
            }
        } else {
            size_t numPeers = 0;

            for(size_t i = 0; i < outgoingPacket.peers.size(); i++) {
                if(serverPeerArray[outgoingPacket.peers[i]] == nullptr)
                    continue;                
                numPeers++;
            }            

            ENetPeer *peers[numPeers];
            size_t index = 0;

            for(size_t i = 0; i < outgoingPacket.peers.size(); i++) {
                if(serverPeerArray[outgoingPacket.peers[i]] == nullptr)
                    continue;                
                peers[index] = serverPeerArray[outgoingPacket.peers[i]];
                index++;
            }

            enet_host_broadcast_selective(server, outgoingPacket.channel, outgoingPacket.payload, peers, numPeers);
        }
    }
}

void Server::handleNetworkEvents(const ServerConfiguration &setupInfo) {
    ENetEvent event;
    bool pollComplete = false;

    while (!pollComplete) {
        if (enet_host_check_events(server, &event) <= 0) {
            if (enet_host_service(server, &event, setupInfo.pollTime) <= 0)
                break;

            pollComplete = true;
        }

        ENetPeer *incomingPeer = event.peer;

        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                char ip[128];
                memset(ip, 0, 128);
                ip[127] = '\0';
                enet_peer_get_ip(event.peer, ip, 128);
                std::string peerIP(ip);
                
                uint16_t port = enet_peer_get_port(event.peer);

                NetworkConnectionEvent ice {
                    .port = port,
                    .peerId = event.peer->incomingPeerID,
                    .IP = peerIP
                };

                connectionEvents->enqueue(ice);

                serverPeerArray[incomingPeer->incomingPeerID] = incomingPeer;
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
            case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: {
                if (serverPeerArray[incomingPeer->incomingPeerID] == nullptr) 
                    break;

                NetworkConnectionEvent iced {
                    .eventType = 0x01,
                    .peerId = incomingPeer->incomingPeerID
                };

                disconnectionEvents->enqueue(iced);

                serverPeerArray[incomingPeer->incomingPeerID] = nullptr;
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                ENetPacket *incomingPacket = event.packet;

                if (incomingPacket == nullptr)
                    break;

                int incomingPacketLength = enet_packet_get_length(incomingPacket);

                if (incomingPacketLength > setupInfo.packetSizeLimit) {
                    enet_packet_dispose(incomingPacket);
                    break;
                }

                NetworkIncomingPacket incomingQueuePacket {
                    .channel = event.channelID,
                    .peerId = incomingPeer->incomingPeerID,
                    .payload = incomingPacket
                };

                incoming->enqueue(incomingQueuePacket);
                break;
            }
            case ENET_EVENT_TYPE_NONE:
            default: {
                break;
            }
        }
    }
}

void Server::update() {
    NetworkIncomingPacket incomingPacket;
    while (incoming->tryDequeue(incomingPacket)) {
        received(incomingPacket);
        if(incomingPacket.payload != nullptr)
            enet_packet_dispose(incomingPacket.payload);
    }

    NetworkConnectionEvent connection;

    while (connectionEvents->tryDequeue(connection)) {
        connected(connection);
    }

    NetworkConnectionEvent disconnection;

    while (disconnectionEvents->tryDequeue(disconnection)) {
        disconnected(disconnection);
    }

    NetworkServerStats stats;

    while (statusUpdates->tryDequeue(stats)) {
        statsUpdate(stats);
    }
}

void Server::stop() {
    runFlag = false;
    
    if (networkThread.joinable()) {
        if (verbosity > 0)
            writeLog("Server stopping...");
        networkThread.join();
    }
}

bool Server::isRunning() const {
    return runFlag;
}

void Server::setupRingBuffersIfNull() {
    incoming = std::make_unique<RingBuffer<NetworkIncomingPacket>>(incomingOutgoingBufferSize);
    outgoing = std::make_unique<RingBuffer<NetworkOutgoingPacket>>(incomingOutgoingBufferSize);
    commands = std::make_unique<RingBuffer<NetworkCommandPacket>>(connectionEventBufferSize);
    connectionEvents = std::make_unique<RingBuffer<NetworkConnectionEvent>>(connectionEventBufferSize);
    disconnectionEvents = std::make_unique<RingBuffer<NetworkConnectionEvent>>(connectionEventBufferSize);
    statusUpdates = std::make_unique<RingBuffer<NetworkServerStats>>(connectionEventBufferSize);
    recycledServerStatBlocks = std::make_unique<RingBuffer<NetworkServerStats>>(connectionEventBufferSize);
}

void Server::drainQueues() {
    if (incoming != nullptr)
        incoming->drain();
    if (outgoing != nullptr)
        outgoing->drain();
    if (commands != nullptr)
        commands->drain();
    if (connectionEvents != nullptr)
        connectionEvents->drain();
    if (disconnectionEvents != nullptr)
        disconnectionEvents->drain();
    if (statusUpdates != nullptr)
        statusUpdates->drain();
    if (recycledServerStatBlocks != nullptr)
        recycledServerStatBlocks->drain();
}

void Server::send(uint16_t peerId, byte *data, size_t size, byte channel, NetworkPacketFlags flags) {
    NetworkOutgoingPacket packet;
    packet.channel = channel;
    packet.numPeers = 1;
    packet.peerId = peerId;
    packet.payload = enet_packet_create(data, size, flags);
    outgoing->enqueue(packet);
}

void Server::broadcast(byte *data, size_t size, byte channel, NetworkPacketFlags flags) {
    NetworkOutgoingPacket packet;
    packet.channel = channel;
    packet.numPeers = 0;
    packet.payload = enet_packet_create(data, size, flags);
    outgoing->enqueue(packet);
}

void Server::broadcastSelective(const std::vector<uint16_t> &peerIds, byte *data, size_t size, byte channel, NetworkPacketFlags flags) {
    NetworkOutgoingPacket packet;
    packet.channel = channel;
    packet.numPeers = peerIds.size();
    packet.peers = peerIds;
    packet.payload = enet_packet_create(data, size, flags);
    outgoing->enqueue(packet);
}

void Server::kick(uint16_t peerId) {
    NetworkCommandPacket command;
    command.peerId = peerId;
    command.type = NetworkCommandType_ServerKickPeer;
    commands->enqueue(command);
}

}

namespace mpp::clients {

Client::Client(const NetworkConfig &config) {
    this->config = config;

    if(this->config.maxChannels < 1)
        this->config.maxChannels = 1;

    if(this->config.maxPacketSize > this->config.bufferSize)
        this->config.maxPacketSize = this->config.bufferSize;

    incomingBuffer.resize(this->config.bufferSize);
    outgoingBuffer.resize(this->config.bufferSize);

    connectionQueue = std::make_unique<RingBuffer<ConnectionInfo>>(1024);
    disconnectionQueue = std::make_unique<RingBuffer<ConnectionInfo>>(1024);
    incomingPacketQueue = std::make_unique<RingBuffer<NetworkIncomingPacket>>(this->config.incomingQueueCapacity);
    outgoingPacketQueue = std::make_unique<RingBuffer<NetworkOutgoingPacket>>(this->config.outgoingQueueCapacity);

    runFlag = false;
}

bool Client::isRunning() const {
    return runFlag;
}

void Client::start() {
    if(networkThread.joinable() || runFlag) {
        writeLog("A worker thread is already running");
        return;
    }

    //Drain queues in case any leftover data is still present from a previous thread
    connectionQueue->drain();
    disconnectionQueue->drain();
    incomingPacketQueue->drain();
    outgoingPacketQueue->drain();

    runFlag = true;

    networkThread = std::thread([this] () { networkUpdate(); });
}

void Client::stop() {
    if(!runFlag)
        return;

    runFlag = false;

    if(networkThread.joinable())
        networkThread.join();
}

void Client::update()
{
    if(connectionQueue->count() > 0) {
        while(connectionQueue->count() > 0) {
            ConnectionInfo info;
            if(connectionQueue->tryDequeue(info)) {
                connected();
            }
        }
    }

    if(disconnectionQueue->count() > 0) {
        while(disconnectionQueue->count() > 0) {
            ConnectionInfo info;
            if(disconnectionQueue->tryDequeue(info)) {
                disconnected();
            }
        }
    }

    if(incomingPacketQueue->count() > 0) {
        while(incomingPacketQueue->count() > 0) {
            NetworkIncomingPacket packetInfo;

            if(incomingPacketQueue->tryDequeue(packetInfo)) {
                //If packet exceeds maximum size, just deallocate it
                if(packetInfo.payload->dataLength > config.maxPacketSize) {
                    enet_packet_dispose(packetInfo.payload);
                } else {
                    received(packetInfo);
                    enet_packet_dispose(packetInfo.payload);
                }
            }
        }
    }
}

void Client::send(byte *data, size_t length, byte channelId, ENetPacketFlag flags) {
    ENetPacket *packet = enet_packet_create(data, length, flags);

    if(packet == nullptr)
        return;
    
    NetworkOutgoingPacket info;
    info.channel = channelId;
    info.payload = packet;
    outgoingPacketQueue->enqueue(info);
}

void Client::networkUpdate() {
    if(enet_initialize() != 0) {
        writeLog("Failed to initialize ENet");
        runFlag = false;
        return;
    }

    ENetAddress address;
    address.port = config.port;
    enet_address_set_hostname(&address, config.hostAddress.c_str());

    ENetHost *client = enet_host_create(nullptr, 1, 0, 0, 0, 0);

    if(client == nullptr) {
        writeLog("Couldn't create client host");
        runFlag = false;
        enet_deinitialize();
        return;
    }

    enet_host_set_checksum_callback(client, &enet_crc64);

    ENetPeer *peer = enet_host_connect(client, &address, config.maxChannels, 0);

    if(peer == nullptr) {
        runFlag = false;
        enet_deinitialize();
        return;
    }

    while(runFlag) {
        if(outgoingPacketQueue->count() > 0) {
            while(outgoingPacketQueue->count() > 0) {
                NetworkOutgoingPacket packetInfo;

                if(outgoingPacketQueue->tryDequeue(packetInfo)) {
                    //If packet size exceeds maximum packet size, don't send but just deallocate
                    if(packetInfo.payload->dataLength > config.maxPacketSize) {
                        enet_packet_dispose(packetInfo.payload);
                    } else {
                        enet_peer_send(peer, packetInfo.channel, packetInfo.payload);
                    }
                }
            }
        }

        bool polled = false;

        ENetEvent netEvent;

        while (!polled) {
            if(enet_host_check_events(client, &netEvent) <= 0) {
                if(enet_host_service(client, &netEvent, 15) <= 0)
                    break;

                polled = true;
            }

            switch (netEvent.type) {
                case ENET_EVENT_TYPE_NONE:
                    break;
                case ENET_EVENT_TYPE_CONNECT: {
                    ConnectionInfo info;
                    connectionQueue->enqueue(info);
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT: {
                    ConnectionInfo info;
                    disconnectionQueue->enqueue(info);
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT: {
                    ConnectionInfo info;
                    disconnectionQueue->enqueue(info);
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE: {
                    NetworkIncomingPacket info;
                    info.channel = netEvent.channelID;
                    info.payload = netEvent.packet;
                    incomingPacketQueue->enqueue(info);
                    break;
                }
            }
        }
    }

    enet_host_flush(client);
    enet_peer_disconnect_now(peer, 0);
    enet_host_destroy(client);
    enet_deinitialize();
}

}