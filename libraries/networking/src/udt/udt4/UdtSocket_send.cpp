//
//  UdtSocket_send.cpp
//  libraries/networking/src/udt/udt4
//
//  Created by Heather Anderson on 2020-06-20.
//  Copyright 2020 Vircadia contributors
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "UdtSocket_send.h"
#include "UdtSocket.h"

using namespace udt4;

UdtSocket_send::UdtSocket_send(UdtSocket_private& socket) : _socket(socket), _socketState(UdtSocketState::Init) {
    _SNDtimer.setSingleShot(true);
    _SNDtimer.setTimerType(Qt::PreciseTimer);
    connect(&_SNDtimer, &QTimer::timeout, this, &UdtSocket_send::SNDevent);

    _EXPtimer.setSingleShot(true);
    _EXPtimer.setTimerType(Qt::PreciseTimer);
    connect(&_EXPtimer, &QTimer::timeout, this, &UdtSocket_send::EXPevent);
}

void UdtSocket_send::configureHandshake(const HandshakePacket& hsPacket, bool resetSequence, unsigned mtu) {
    if (resetSequence) {
        _lastAckPacketID = hsPacket._initPktSeq;
        _sendPacketID = hsPacket._initPktSeq;
	}
    _mtu = mtu;
    _isDatagram = hsPacket._sockType == SocketType::DGRAM;
    _flowWindowSize = hsPacket._maxFlowWinSize;
}

void UdtSocket_send::startupInit() {
    _flagRecentReceivedPacket = false;
    _flagRecentEXPevent = false;
    _flagRecentSNDevent = false;
    _flagSendDisconnect = false;
    _lastReceiveTime.start();
    _msgPartialSend.reset();
    _expCount = 1;
    _messageSequence = 0UL;
    _sentAck2 = 0UL;
    _ACK2SentTimer.setRemainingTime(0); // default expired
    _receivedPacketList.clear();
    _sendLossList.clear();
    _sendPktPend.clear();
    resetEXP();
}

// the main event loop for the "send" side of the socket, this controls the behavior and permitted actions
void UdtSocket_send::run() {
    startupInit();
	for(;;) {
        QMutexLocker guard(&_eventMutex);
        while (!processEvent(guard)) {
            if (_sendState == SendState::Closed) {
                // socket is closed, leave this thread
                return;
            }
            _eventCondition.wait(&_eventMutex);
        }
    }
}

void UdtSocket_send::setState(UdtSocketState newState) {
    bool shouldBeRunning = false;
    switch (newState) {
        case UdtSocketState::Connected:
        case UdtSocketState::HalfClosed:
            shouldBeRunning = true;
            break;
    }

    QMutexLocker guard(&_eventMutex);
    _socketState = newState;
    _eventCondition.notify_all();
    if (shouldBeRunning && !isRunning()) {
        start();
    }
}

void UdtSocket_send::resetReceiveTimer() {
    QMutexLocker guard(&_eventMutex);
    _flagRecentReceivedPacket = true;
    _eventCondition.notify_all();
}

void UdtSocket_send::EXPevent() {
    QMutexLocker guard(&_eventMutex);
    _flagRecentEXPevent = true;
    _eventCondition.notify_all();
}

void UdtSocket_send::SNDevent() {
    QMutexLocker guard(&_eventMutex);
    _flagRecentSNDevent = true;
    _eventCondition.notify_all();
}

void UdtSocket_send::queueDisconnect() {
    QMutexLocker guard(&_eventMutex);
    _flagSendDisconnect = true;
    _eventCondition.notify_all();
}

void UdtSocket_send::sendMessage(ByteSlice content, QDeadlineTimer expireTime) {
    MessageEntryPointer message = MessageEntryPointer::create(content);
    message->expireTime = expireTime;

    QMutexLocker guard(&_eventMutex);
    _pendingMessages.append(message);
    _eventCondition.notify_all();
}

void UdtSocket_send::packetReceived(const Packet& udtPacket, const QElapsedTimer& timeReceived) {
    ReceivedPacket packet(udtPacket, timeReceived);
    QMutexLocker guard(&_eventMutex);
    _receivedPacketList.append(packet);
    _eventCondition.notify_all();
}

// the main event loop for the "send" side of the socket, this controls the behavior and permitted actions
bool UdtSocket_send::processEvent(QMutexLocker& eventGuard) {

    if (_flagRecentReceivedPacket && _sendState != SendState::Shutdown) {
        _flagRecentReceivedPacket = false;
        _flagRecentEXPevent = false;
        eventGuard.unlock();
        _expCount = 1;
        resetEXP();
    }

    bool canSendPacket = false;
    switch (_sendState) {
	case SendState::Idle: // not waiting for anything, can send immediately
        canSendPacket = true;
        break;
	case SendState::ProcessDrop: // immediately re-process any drop list requests
        eventGuard.unlock();
		_sendState = reevalSendState(); // try to reconstruct what our state should be if it wasn't sendStateProcessDrop
        if (!processSendLoss() || (static_cast<quint32>(_sendPacketID) % 16) == 0) {
			processSendExpire();
		}
		return true;
	}

    switch (_socketState) {
    case UdtSocketState::Connected: // this is the expected state while we are running
        break;
    case UdtSocketState::HalfClosed:
        if (_sendState != SendState::Shutdown) {
            _sendState = SendState::Shutdown;
            _flagRecentEXPevent = false;
            eventGuard.unlock();
            if (_EXPtimer.isActive()) {  // don't process EXP events if we're shutting down
                _EXPtimer.stop();
            }
            return true;
        }
        break;
    default: // not a running state
        _sendState = SendState::Closed;
        return false;
    }

    if (canSendPacket) {
        if (_msgPartialSend != nullptr) {  // we have a partial message waiting, try to send more of it now
            eventGuard.unlock();
            processDataMsg(false);
            return true;
        }
        if (!_pendingMessages.isEmpty()) {
            _msgPartialSend = _pendingMessages.takeFirst();
            eventGuard.unlock();
            processDataMsg(true);
            return true;
        }
        if (_flagSendDisconnect) {
            _flagSendDisconnect = false;
            eventGuard.unlock();
            Packet shutdownPacket;
            shutdownPacket._type = PacketType::Shutdown;
            _socket.sendPacket(shutdownPacket);
            return true;
        }
    }

    if (!_receivedPacketList.isEmpty()) {
        ReceivedPacket recvPacket = _receivedPacketList.takeFirst();
        switch (recvPacket.udtPacket._type) {
        case PacketType::Ack:
			ingestAck(ACKPacket(recvPacket.udtPacket), recvPacket.timeReceived);
            break;
        case PacketType::Nak:
            ingestNak(NAKPacket(recvPacket.udtPacket), recvPacket.timeReceived);
            break;
        case PacketType::Congestion:
            ingestCongestion(recvPacket.udtPacket, recvPacket.timeReceived);
            break;
        }
		_sendState = reevalSendState();
    }

    if (_flagRecentEXPevent) {
        _flagRecentEXPevent = false;
        eventGuard.unlock();
        processExpEvent();
        return true;
    }

    if (_flagRecentSNDevent) {
        _flagRecentSNDevent = false;
        if (_sendState == SendState::Sending) {
            eventGuard.unlock();
            _sendState = reevalSendState();
            if (!processSendLoss() || (static_cast<quint32>(_sendPacketID) % 16) == 0) {
				processSendExpire();
			}
            return true;
        }
	}

    // no events seen to process
    return false;
}

void UdtSocket_send::setPacketSendPeriod(std::chrono::milliseconds snd) { // exported
	// check to see if we have a bandwidth limit here
	unsigned maxBandwidth = _socket.getMaxBandwidth();
	if (maxBandwidth > 0) {
		std::chrono::milliseconds minSP(static_cast<quint64>(std::chrono::milliseconds{ std::chrono::seconds{1} }.count() / (static_cast<double>(maxBandwidth)/_mtu)));
		if (snd < minSP) {
			snd = minSP;
		}
	}

	_sndPeriod.store(snd.count());
}

// generally set by congestion control
void UdtSocket_send::setCongestionWindow(unsigned pkt) {
    _congestWindow.store(pkt);
}

// generally set by congestion control
void UdtSocket_send::setRTOperiod(std::chrono::milliseconds rto) {
    _rtoPeriod.store(rto.count());
}

UdtSocket_send::SendState UdtSocket_send::reevalSendState() const {
    if (_SNDtimer.isActive()) {
		return SendState::Sending;
	}

	// Do we have too many unacknowledged packets for us to send any more?
	if (!_sendPktPend.empty()) {
		unsigned congestWindow = _congestWindow.load();
		uint cwnd = _flowWindowSize;
		if (cwnd > congestWindow) {
			cwnd = congestWindow;
		}
		if (cwnd >= _sendPktPend.size()) {
			return SendState::Waiting;
		}
	}
	return SendState::Idle;
}

// try to pack a new data packet and send it
void UdtSocket_send::processDataMsg(bool isFirst) {
	while (_msgPartialSend != nullptr) {
        MessageEntryPointer partialSend = _msgPartialSend;
		DataPacket::MessagePosition state = DataPacket::MessagePosition::Only;
        if (_isDatagram) {
			if(isFirst) {
				state = DataPacket::MessagePosition::First;
			} else {
				state = DataPacket::MessagePosition::Middle;
			}
		}
        if (isFirst || !_isDatagram) {
			_messageSequence++;
		}

		unsigned msgLen = static_cast<unsigned>(partialSend->content.length());
		if (msgLen >= _mtu) {
			// we are full -- send what we can and leave the rest
			DataPacket dataPacket;
            dataPacket._packetID = _sendPacketID;
            dataPacket._messagePosition = state;
            dataPacket._isOrdered = !_isDatagram;
            dataPacket._messageNumber = _messageSequence;
			if (msgLen == _mtu) {
                dataPacket._contents = partialSend->content;
				_msgPartialSend.reset();
			} else {
                dataPacket._contents = partialSend->content.substring(0, _mtu);
				_msgPartialSend->content = partialSend->content.substring(_mtu);
			}
			_sendPacketID++;

            SendPacketEntryPointer dataPacketEntry = SendPacketEntryPointer::create();
            dataPacketEntry->packet = dataPacket;
            dataPacketEntry->sendTime = partialSend->sendTime;
            dataPacketEntry->expireTime = partialSend->expireTime;

			sendDataPacket(dataPacketEntry, false);
			return;
		}

		// we are not full -- send only if this is a datagram or there's nothing obvious left
		if (_isDatagram) {
			if(isFirst) {
				state = DataPacket::MessagePosition::Only;
			} else {
				state = DataPacket::MessagePosition::Last;
			}
		} else {
            QMutexLocker guard(&_eventMutex);
            if(!_pendingMessages.isEmpty()) {
                MessageEntryPointer morePartialSend = _pendingMessages.takeFirst();
                _msgPartialSend->content = _msgPartialSend->content.concat(morePartialSend->content);
				continue;
			}
		}

		partialSend = _msgPartialSend;
		DataPacket dataPacket;
        dataPacket._packetID = _sendPacketID;
        dataPacket._contents = partialSend->content;
        dataPacket._messagePosition = state;
        dataPacket._isOrdered = !_isDatagram;
        dataPacket._messageNumber = _messageSequence;
        _msgPartialSend.reset();
        _sendPacketID++;

        SendPacketEntryPointer dataPacketEntry = SendPacketEntryPointer::create();
        dataPacketEntry->packet = dataPacket;
        dataPacketEntry->sendTime = partialSend->sendTime;
        dataPacketEntry->expireTime = partialSend->expireTime;

		sendDataPacket(dataPacketEntry, false);
		return;
	}
}

// If the sender's loss list is not empty, retransmit the first packet in the list and remove it from the list.
bool UdtSocket_send::processSendLoss() {
    if (_sendLossList.empty() || _sendPktPend.empty()) {
		return false;
	}

	SendPacketEntryPointer dataPacketEntry;
	for(;;) {
        PacketIDSet::iterator minLossFind = findFirstEntry(_sendLossList, _lastAckPacketID, _sendPacketID);
		if (minLossFind == _sendLossList.end()) {
			// empty loss list? shouldn't really happen as we don't keep empty lists, but check for it anyhow
			return false;
		}

        _sendLossList.erase(minLossFind);

        SendPacketEntryMap::const_iterator lookup = _sendPktPend.find(*minLossFind);
		if (lookup == _sendPktPend.end()) {
			// can't find record of this packet, not much we can do really
			continue;
		}

		if (dataPacketEntry->expireTime.hasExpired()) {
			// this packet has expired, ignore
			continue;
		}

		break;
	}

	sendDataPacket(dataPacketEntry, true);
	return true;
}

// evaluate our pending packet list to see if we have any expired messages
bool UdtSocket_send::processSendExpire() {
	if (_sendPktPend.empty()) {
		return false;
	}

    // make a copy of _sendPktPend as we'll be mucking the heck over its contents and we don't want to invalidate our iterators
    SendPacketEntryMap ourSendPktPend(_sendPktPend);

    // don't really care (yet) what order we process these packets in so we'll just do it "arbitrary" order without using findFirstEntry
    for (SendPacketEntryMap::const_iterator evalIterator = ourSendPktPend.begin(); evalIterator != ourSendPktPend.end(); evalIterator++) {
        
        SendPacketEntryMap::iterator realEvalIterator = _sendPktPend.find(evalIterator->first);
        if (realEvalIterator == _sendPktPend.end()) { // has this packet already been dropped?
            continue;
        }
        
        SendPacketEntryPointer thisEntry = evalIterator->second;
		if (thisEntry->expireTime.hasExpired()) {

            // this message has expired, drop it
            SequenceNumber messageNumber = thisEntry->packet._messageNumber;

            MessageDropRequestPacket dropMessage;
            dropMessage._messageID = messageNumber;
            dropMessage._firstPacketID = evalIterator->first;
            dropMessage._lastPacketID = evalIterator->first;

			// find the other packets in this message
            for (SendPacketEntryMap::const_iterator collectIterator = ourSendPktPend.begin(); collectIterator != ourSendPktPend.end(); collectIterator++) {
                if (collectIterator->second->packet._messageNumber == messageNumber) {
                    const PacketID& packetID = collectIterator->first;
					if (packetID < dropMessage._firstPacketID) {
                        dropMessage._firstPacketID = packetID;
					}
					if (packetID > dropMessage._lastPacketID) {
                        dropMessage._lastPacketID = packetID;
					}
                    PacketIDSet::iterator findLoss = _sendLossList.find(packetID);
                    if (findLoss != _sendLossList.end()) {
                        _sendLossList.erase(findLoss);
                    }
                    _sendPktPend.erase(packetID);
                }
			}

            _socket.sendPacket(dropMessage.toPacket());
			return true;
		}
	}
	return false;
}

// we have a packed packet and a green light to send, so lets send this and mark it
void UdtSocket_send::sendDataPacket(SendPacketEntryPointer dataPacketEntry, bool isResend) {
    _sendPktPend.insert(SendPacketEntryMap::value_type(dataPacketEntry->packet._packetID, dataPacketEntry));
    _socket.getCongestionControl().onDataPktSent(dataPacketEntry->packet._packetID);
    _socket.sendPacket(dataPacketEntry->packet.toPacket());

	// have we exceeded our recipient's window size?
	_sendState = reevalSendState();
	if (_sendState == SendState::Waiting) {
		return;
	}

	if (!isResend && (static_cast<quint32>(dataPacketEntry->packet._packetID) % 16) == 0) {
		processSendExpire();
		return;
	}

	std::chrono::milliseconds snd(_sndPeriod.load());
	if (snd.count() > 0) {
		_SNDtimer.start(snd);
		_sendState = SendState::Sending;
	}
}

bool UdtSocket_send::assertValidSentPktID(const char* pktType, const PacketID& packetID) {
    if (_sendPacketID.blindDifference(packetID) < 0) {
        _socket.requestShutdown(UdtSocketState::Corrupted,
			QString("FAULT: Received an %1 for packet %2, but the largest packet we've sent has been %3")
                .arg(pktType, static_cast<quint32>(packetID), static_cast<quint32>(_sendPacketID)));
		return false;
	}
	return true;
}

// ingestAck is called to process an ACK packet
void UdtSocket_send::ingestAck(const ACKPacket& ackPacket, const QElapsedTimer& timeReceived) {
	// Update the largest acknowledged sequence number.

    if (ackPacket._ackType == ACKPacket::AckType::Light) {
	    PacketID lastPacketReceived = ackPacket._lastPacketReceived;
        if (!assertValidSentPktID("ACK", lastPacketReceived)) {
		    return;
	    }
        qint32 diff = lastPacketReceived.blindDifference(_lastAckPacketID);
	    if (diff > 0) {
		    _flowWindowSize += diff;
		    _lastAckPacketID = lastPacketReceived;
	    }
    }

	// Send back an ACK2 with the same ACK sequence number in this ACK.
    if (_ACK2SentTimer.hasExpired() && ackPacket._ackSequence == _sentAck2) {
        _sentAck2 = ackPacket._ackSequence;

        Packet ack2Packet;
        ack2Packet._type = PacketType::Ack2;
        ack2Packet._additionalInfo = static_cast<quint32>(ackPacket._ackSequence);
        _socket.sendPacket(ack2Packet);
        _ACK2SentTimer.setRemainingTime(UdtSocket::SYN, Qt::PreciseTimer);
	}

	PacketID lastPacketReceived = ackPacket._lastPacketReceived;
        if (!assertValidSentPktID("ACK", lastPacketReceived)) {
		return;
	}
	if (_lastAckPacketID <= lastPacketReceived) {
		return;
	}

	PacketID oldAckSeq = _lastAckPacketID;
	_flowWindowSize = ackPacket._availBufferSize;
	_lastAckPacketID = lastPacketReceived;

	// Update RTT and RTTVar.
	_socket.applyRTT(ackPacket._rtt);

	// Update flow window size.
	if (ackPacket._ackType == ACKPacket::AckType::Full) {
		_socket.applyReceiveRates(ackPacket._packetReceiveRate, ackPacket._estimatedLinkCapacity);
	}

	_socket.getCongestionControl().onACK(lastPacketReceived);

	// Update packet arrival rate: A = (A * 7 + a) / 8, where a is the value carried in the ACK.
	// Update estimated link capacity: B = (B * 7 + b) / 8, where b is the value carried in the ACK.

	// Update sender's buffer (by releasing the buffer that has been acknowledged).
	for(;;) {
        SendPacketEntryMap::iterator minLossFind = findFirstEntry(_sendPktPend, oldAckSeq, _sendPacketID);
        if(minLossFind == _sendPktPend.end() || minLossFind->first >= lastPacketReceived) {
			break;
		}
        _sendPktPend.erase(minLossFind);
	}

	// Update sender's loss list (by removing all those that has been acknowledged).
	for(;;) {
        PacketIDSet::iterator minLossFind = findFirstEntry(_sendLossList, oldAckSeq, _sendPacketID);
        if(minLossFind == _sendLossList.end() || *minLossFind >= lastPacketReceived) {
			break;
		}
        _sendLossList.erase(minLossFind);
	}
}

// ingestNak is called to process an NAK packet
void UdtSocket_send::ingestNak(const NAKPacket& nakPacket, const QElapsedTimer& timeReceived) {
    QList<PacketID> newLossList;
	for (NAKPacket::IntegerList::const_iterator trans = nakPacket._lossData.begin(); trans != nakPacket._lossData.end(); trans++) {
		quint32 thisEntry = *trans;
		if ((thisEntry&0x80000000) != 0) {
			PacketID thisPacketID(thisEntry);
            trans++;
            if(trans == nakPacket._lossData.end()) {
                _socket.requestShutdown(UdtSocketState::Corrupted,
					QString("FAULT: While unpacking a NAK, the last entry (%1) was describing a start-of-range").arg(thisEntry));
				return;
			}
            if (!assertValidSentPktID("NAK", thisPacketID)) {
				return;
			}
            quint32 lastEntry = *trans;
			if ((lastEntry&0x80000000) != 0) {
                _socket.requestShutdown(UdtSocketState::Corrupted,
					QString("FAULT: While unpacking a NAK, a start-of-range (%1) was followed by another start-of-range (%2)").arg(thisEntry, lastEntry));
				return;
			}
			PacketID lastPacketID(lastEntry);
			if (!assertValidSentPktID("NAK", lastPacketID)) {
				return;
			}
			for (PacketID span = thisPacketID; span != lastPacketID; span++) {
				newLossList.append(span);
                _sendLossList.insert(span);
			}
		} else {
			PacketID thisPacketID(thisEntry);
			if (!assertValidSentPktID("NAK", thisPacketID)) {
				return;
			}
			newLossList.append(thisPacketID);
            _sendLossList.insert(thisPacketID);
		}
	}

	_socket.getCongestionControl().onNAK(newLossList);
	_sendState = SendState::ProcessDrop; // immediately restart transmission
}

// ingestCongestion is called to process a (retired?) Congestion packet
void UdtSocket_send::ingestCongestion(const Packet& udtPacket, const QElapsedTimer& timeReceived) {
	// One way packet delay is increasing, so decrease the sending rate
	// this is very rough (not atomic, doesn't inform congestion) but this is a deprecated message in any case
    _sndPeriod.store(_sndPeriod.load() * 1125 / 1000);
	//m_iLastDecSeq = s.sendPktSeq;
}

void UdtSocket_send::resetEXP() {
    _lastReceiveTime.start();

	std::chrono::milliseconds nextExpDurn;
    std::chrono::milliseconds rtoPeriod(_rtoPeriod.load());
	if (rtoPeriod.count() > 0) {
		nextExpDurn = rtoPeriod;
	} else {
        std::chrono::microseconds rtt, rttVariance;
        _socket.getRTT(rtt, rttVariance);

        nextExpDurn = std::chrono::milliseconds(_expCount * (rtt.count() + 4 * rttVariance.count()) / 1000) + UdtSocket::SYN;
        std::chrono::milliseconds minExpTime(_expCount * MIN_EXP_INTERVAL);
		if (nextExpDurn.count() < minExpTime.count()) {
			nextExpDurn = minExpTime;
		}
	}
	_EXPtimer.start(nextExpDurn);
}

// we've just had the EXP timer expire, see what we can do to recover this
void UdtSocket_send::processExpEvent() {

	// Haven't receive any information from the peer, is it dead?!
	// timeout: at least 16 expirations and must be greater than 10 seconds
    if ((_expCount > 16) && (std::chrono::milliseconds(_lastReceiveTime.elapsed()) > MIN_CONNECTION_TIMEOUT)) {
		// Connection is broken.
        _socket.requestShutdown(UdtSocketState::Timeout, QString("Timeout - last packet received %1 seconds ago").arg(_lastReceiveTime.elapsed() / 1000.0));
		return;
	}

	// sender: Insert all the packets sent after last received acknowledgement into the sender loss list.
	// recver: Send out a keep-alive packet
	if (!_sendPktPend.empty()) {
		if (_sendLossList.empty()) {
			// resend all unacknowledged packets on timeout, but only if there is no packet in the loss list
			for(PacketID span = _lastAckPacketID + 1; span != _sendPacketID + 1; span++) {
                _sendLossList.insert(span);
			}
		}
		_socket.getCongestionControl().onTimeout();
		_sendState = SendState::ProcessDrop; // immediately restart transmission
	} else {
        Packet keepalivePacket;
        keepalivePacket._type = PacketType::Keepalive;
        _socket.sendPacket(keepalivePacket);
	}

	_expCount++;
	// Reset last response time since we just sent a heart-beat.
	resetEXP();
}
