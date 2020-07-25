//
//  Multiplexer.inl
//  libraries/networking/src/udt/udt4
//
//  Created by Heather Anderson on 2020-05-25.
//  Copyright 2020 Vircadia contributors
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#pragma once

#ifndef hifi_udt4_Multiplexer_inl
#define hifi_udt4_Multiplexer_inl

#include "Multiplexer.h"

namespace udt4 {

template <class P>
inline PacketEvent<P>::PacketEvent(const P& p, const QHostAddress& address, quint32 port) : packet(p), peerAddress(address), peerPort(port) {
    age.start();
}

inline void UdtMultiplexer::moveToReadThread(QObject* object) {
    object->moveToThread(&_readThread);
}

inline void UdtMultiplexer::moveToWriteThread(QObject* object) {
    object->moveToThread(&_writeThread);
}

inline QHostAddress UdtMultiplexer::serverAddress() const {
    return _serverAddress;
}

inline QAbstractSocket::SocketError UdtMultiplexer::serverError() const {
    return _udpSocket.error();
}

inline quint16 UdtMultiplexer::serverPort() const {
    return _serverPort;
}

inline QString UdtMultiplexer::errorString() const {
    return _udpSocket.errorString();
}

inline bool UdtMultiplexer::isLive() const {
    return _udpSocket.isOpen();
}

} /* namespace udt4 */
#endif /* hifi_udt4_Multiplexer_inl */