/*
 *  Copyright 2021 (c) Microsoft Corporation.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * LinkProber.cpp
 *
 *  Created on: Oct 4, 2020
 *      Author: tamer
 */

#include <netpacket/packet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <malloc.h>

#include <boost/bind/bind.hpp>

#include "common/MuxLogger.h"
#include "LinkProber.h"
#include "common/MuxException.h"

namespace link_prober
{

//
// Berkeley Packet Filter program that captures incoming ICMP traffic
//
SockFilter LinkProber::mIcmpFilter[] = {
    [0]  = {.code = 0x28, .jt = 0, .jf = 0,  .k = 0x0000000c},
    [1]  = {.code = 0x15, .jt = 0, .jf = 10, .k = 0x00000800},
    [2]  = {.code = 0x20, .jt = 0, .jf = 0,  .k = 0x0000001a},
    [3]  = {.code = 0x15, .jt = 0, .jf = 8,  .k = 0x00000000},
    [4]  = {.code = 0x30, .jt = 0, .jf = 0,  .k = 0x00000017},
    [5]  = {.code = 0x15, .jt = 0, .jf = 6,  .k = 0x00000001},
    [6]  = {.code = 0x28, .jt = 0, .jf = 0,  .k = 0x00000014},
    [7]  = {.code = 0x45, .jt = 4, .jf = 0,  .k = 0x00001fff},
    [8]  = {.code = 0xb1, .jt = 0, .jf = 0,  .k = 0x0000000e},
    [9]  = {.code = 0x50, .jt = 0, .jf = 0,  .k = 0x0000000e},
    [10] = {.code = 0x15, .jt = 0, .jf = 1,  .k = 0x00000000},
    [11] = {.code = 0x6,  .jt = 0, .jf = 0,  .k = 0x00040000},
    [12] = {.code = 0x6,  .jt = 0, .jf = 0,  .k = 0x00000000},
};

//
// ---> LinkProber(
//          common::MuxPortConfig &muxPortConfig,
//          boost::asio::io_service &ioService,
//          LinkProberStateMachineBase &linkProberStateMachine
//      );
//
// class constructor
//
LinkProber::LinkProber(
    common::MuxPortConfig &muxPortConfig,
    boost::asio::io_service &ioService,
    LinkProberStateMachineBase *linkProberStateMachinePtr
) :
    mMuxPortConfig(muxPortConfig),
    mIoService(ioService),
    mLinkProberStateMachinePtr(linkProberStateMachinePtr),
    mStrand(mIoService),
    mDeadlineTimer(mIoService),
    mSuspendTimer(mIoService),
    mStream(mIoService)
{
    try {
        mSockFilterPtr = std::shared_ptr<SockFilter> (
            new SockFilter[sizeof(mIcmpFilter) / sizeof(*mIcmpFilter)],
            std::default_delete<SockFilter[]>()
        );
        memcpy(mSockFilterPtr.get(), mIcmpFilter, sizeof(mIcmpFilter));

        mSockFilterProg.len = sizeof(mIcmpFilter) / sizeof(*mIcmpFilter);
        mSockFilterProg.filter = mSockFilterPtr.get();
    }
    catch (const std::bad_alloc& ex) {
        std::ostringstream errMsg;
        errMsg << "Failed allocate memory. Exception details: " << ex.what();

        throw MUX_ERROR(BadAlloc, errMsg.str());
    }
}

//
// ---> initialize();
//
// initialize link prober sockets and builds ICMP packet
//
void LinkProber::initialize()
{
    mSocket = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK, IPPROTO_ICMP);
    if (mSocket < 0) {
        std::ostringstream errMsg;
        errMsg << "Failed to open socket with '" << strerror(errno) << "'"
               << std::endl;
        throw MUX_ERROR(SocketError, errMsg.str());
    }

    SockAddrLinkLayer addr;
    memset(&addr, 0, sizeof(addr));

    addr.sll_ifindex = if_nametoindex(mMuxPortConfig.getPortName().c_str());
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ALL);
    if (bind(mSocket, (struct sockaddr *) &addr, sizeof(addr))) {
        std::ostringstream errMsg;
        errMsg << "Failed to bind to interface '" << mMuxPortConfig.getPortName() << "' with '"
               << strerror(errno) << "'" << std::endl;
        throw MUX_ERROR(SocketError, errMsg.str());
    }

    mSockFilterPtr.get()[3].k = mMuxPortConfig.getBladeIpv4Address().to_v4().to_uint();
    if (setsockopt(mSocket, SOL_SOCKET, SO_ATTACH_FILTER, &mSockFilterProg, sizeof(mSockFilterProg)) != 0) {
        std::ostringstream errMsg;
        errMsg << "Failed to attach filter with '" << strerror(errno) << "'"
               << std::endl;
        throw MUX_ERROR(SocketError, errMsg.str());
    }

    mStream.assign(mSocket);
    initializeSendBuffer();

    startInitRecv();
}

//
// ---> startProbing();
//
// start sending ICMP ECHOREQUEST packets
//
void LinkProber::startProbing()
{
    MUXLOGDEBUG(mMuxPortConfig.getPortName());

    mStream.cancel();
    sendHeartbeat();
    startRecv();
    startTimer();
}

//
// ---> suspendTxProbes(uint32_t suspendTime_msec);
//
// suspend sending ICMP ECHOREQUEST packets
//
void LinkProber::suspendTxProbes(uint32_t suspendTime_msec)
{
    MUXLOGDEBUG(mMuxPortConfig.getPortName());

    mSuspendTimer.expires_from_now(boost::posix_time::milliseconds(suspendTime_msec));
    mSuspendTimer.async_wait(mStrand.wrap(boost::bind(
        &LinkProber::handleSuspendTimeout,
        this,
        boost::asio::placeholders::error
    )));

    mSuspendTx = true;
}

//
// ---> resumeTxProbes();
//
// resume sending ICMP ECHOREQUEST packets
//
void LinkProber::resumeTxProbes()
{
    MUXLOGDEBUG(mMuxPortConfig.getPortName());

    mSuspendTimer.cancel();
}

//
// ---> updateEthernetFrame();
//
// update Ethernet frame of Tx Buffer
//
void LinkProber::updateEthernetFrame()
{
    boost::asio::io_service &ioService = mStrand.context();
    ioService.post(mStrand.wrap(boost::bind(&LinkProber::handleUpdateEthernetFrame, this)));
}

//
// ---> probePeerTor();
//
// send an early HB to peer ToR
//
void LinkProber::probePeerTor()
{
    boost::asio::io_service &ioService = mStrand.context();
    ioService.post(mStrand.wrap(boost::bind(&LinkProber::sendHeartbeat, this)));
}

//
// ---> sendPeerSwitchCommand();
//
// send send peer switch command
//
void LinkProber::sendPeerSwitchCommand()
{
    boost::asio::io_service &ioService = mStrand.context();
    ioService.post(mStrand.wrap(boost::bind(&LinkProber::handleSendSwitchCommand, this)));
}

//
// ---> handleUpdateEthernetFrame();
//
// update Ethernet frame of Tx Buffer
//
void LinkProber::handleUpdateEthernetFrame()
{
    initializeSendBuffer();
}

//
// ---> handleSendSwitchCommand();
//
// send switch command to peer ToR
//
void LinkProber::handleSendSwitchCommand()
{
    resetTxBufferTlv();
    appendTlvCommand(Command::COMMAND_SWITCH_ACTIVE);
    appendTlvSentinel();

    size_t totalPayloadSize = mTxPacketSize - mPacketHeaderSize;
    iphdr *ipHeader = reinterpret_cast<iphdr *> (mTxBuffer.data() + sizeof(ether_header));
    icmphdr *icmpHeader = reinterpret_cast<icmphdr *> (mTxBuffer.data() + sizeof(ether_header) + sizeof(iphdr));
    computeChecksum(icmpHeader, sizeof(icmphdr) + totalPayloadSize);
    ipHeader->tot_len = htons(sizeof(iphdr) + sizeof(icmphdr) + totalPayloadSize);

    sendHeartbeat();

    resetTxBufferTlv();
    appendTlvSentinel();
    totalPayloadSize = mTxPacketSize - mPacketHeaderSize;
    computeChecksum(icmpHeader, sizeof(icmphdr) + totalPayloadSize);
    ipHeader->tot_len = htons(sizeof(iphdr) + sizeof(icmphdr) + totalPayloadSize);

    // inform the composite state machine about commend send completion
    boost::asio::io_service::strand &strand = mLinkProberStateMachinePtr->getStrand();
    boost::asio::io_service &ioService = strand.context();
    ioService.post(strand.wrap(boost::bind(
        static_cast<void (LinkProberStateMachineBase::*) (SwitchActiveCommandCompleteEvent&)>
            (&LinkProberStateMachineBase::processEvent),
        mLinkProberStateMachinePtr,
        LinkProberStateMachineBase::getSwitchActiveCommandCompleteEvent()
    )));
}

//
// ---> sendHeartbeat()
//
// send ICMP ECHOREQUEST packet
//
void LinkProber::sendHeartbeat()
{
    MUXLOGTRACE(mMuxPortConfig.getPortName());

    // check if suspend timer is running
    if (!mSuspendTx) {
        updateIcmpSequenceNo();
        boost::system::error_code errorCode;
        mStream.write_some(boost::asio::buffer(mTxBuffer.data(), mTxPacketSize), errorCode);

        if (errorCode) {
            MUXLOGTRACE(mMuxPortConfig.getPortName() + ": Failed to send heartbeat!");
        } else {
            MUXLOGTRACE(mMuxPortConfig.getPortName() + ": Done sending data");
        }
    }
}

//
// ---> handleTlvCommandRecv(Tlv *tlvPtr,, bool isPeer);
//
// handle packet reception
//
void LinkProber::handleTlvCommandRecv(
    Tlv *tlvPtr,
    bool isPeer
)
{
    if (isPeer) {
        if (tlvPtr->command == static_cast<uint8_t> (Command::COMMAND_SWITCH_ACTIVE)) {
            boost::asio::io_service::strand &strand = mLinkProberStateMachinePtr->getStrand();
            boost::asio::io_service &ioService = strand.context();
            ioService.post(strand.wrap(boost::bind(
                static_cast<void (LinkProberStateMachineBase::*) (SwitchActiveRequestEvent&)>
                    (&LinkProberStateMachineBase::processEvent),
                mLinkProberStateMachinePtr,
                LinkProberStateMachineBase::getSwitchActiveRequestEvent()
            )));
        }
    }
}

//
// ---> handleRecv(const boost::system::error_code& errorCode, size_t bytesTransferred);
//
// handle packet reception
//
void LinkProber::handleRecv(
    const boost::system::error_code& errorCode,
    size_t bytesTransferred
)
{
    MUXLOGDEBUG(mMuxPortConfig.getPortName());

    if (errorCode != boost::asio::error::operation_aborted) {
        iphdr *ipHeader = reinterpret_cast<iphdr *> (mRxBuffer.data() + sizeof(ether_header));
        icmphdr *icmpHeader = reinterpret_cast<icmphdr *> (
            mRxBuffer.data() + sizeof(ether_header) + sizeof(iphdr)
        );

        MUXLOGTRACE(boost::format("%s: Got data from: %s, size: %d") %
            mMuxPortConfig.getPortName() %
            boost::asio::ip::address_v4(ntohl(ipHeader->saddr)).to_string() %
            (bytesTransferred - sizeof(iphdr) - sizeof(ether_header))
        );

        IcmpPayload *icmpPayload = reinterpret_cast<IcmpPayload *> (
            mRxBuffer.data() + mPacketHeaderSize
        );

        if (ntohl(icmpPayload->cookie) == IcmpPayload::getCookie() &&
            ntohl(icmpPayload->version) <= IcmpPayload::getVersion() &&
            ntohs(icmpHeader->un.echo.id) == mMuxPortConfig.getServerId()) {
            MUXLOGTRACE(boost::format("%s: Valid ICMP Packet from %s") %
                mMuxPortConfig.getPortName() %
                mMuxPortConfig.getBladeIpv4Address().to_string()
            );
            bool isMatch = (memcmp(icmpPayload->uuid, IcmpPayload::getGuidData(), sizeof(icmpPayload->uuid)) == 0);
            if (isMatch) {
                MUXLOGTRACE(boost::format("%s: Matching Guid") % mMuxPortConfig.getPortName());
                // echo reply for an echo request generated by this/active ToR
                mRxSelfSeqNo = mTxSeqNo;
                mLinkProberStateMachinePtr->postLinkProberStateEvent(LinkProberStateMachineBase::getIcmpSelfEvent());
            } else {
                mRxPeerSeqNo = mTxSeqNo;
                mLinkProberStateMachinePtr->postLinkProberStateEvent(LinkProberStateMachineBase::getIcmpPeerEvent());
            }

            size_t nextTlvOffset = mTlvStartOffset;
            size_t nextTlvSize = 0;
            bool stopProcessTlv = false;
            while ((nextTlvSize = findNextTlv(nextTlvOffset, bytesTransferred)) > 0 && !stopProcessTlv) {
                Tlv *nextTlvPtr = reinterpret_cast<Tlv *> (mRxBuffer.data() + nextTlvOffset);
                switch (nextTlvPtr->tlvhead.type) {
                    case TlvType::TLV_COMMAND: {
                        handleTlvCommandRecv(nextTlvPtr, !isMatch);
                        break;
                    }
                    case TlvType::TLV_SENTINEL: {
                        // sentinel TLV, stop processing
                        stopProcessTlv = true;
                        break;
                    }
                    default: {
                        // try to skip unknown TLV with valid length(>0)
                        stopProcessTlv = (nextTlvSize == sizeof(Tlv));
                        break;
                    }
                }
                nextTlvOffset += nextTlvSize;
            }

            if (nextTlvOffset < bytesTransferred) {
                size_t BytesNotProcessed = bytesTransferred - nextTlvOffset;
                MUXLOGTRACE(boost::format("%s: %d bytes in RxBuffer not processed") %
                    mMuxPortConfig.getPortName() %
                    BytesNotProcessed
                );
            }
        } else {
            // Unknown ICMP packet, ignore.
        }
        // start another receive to consume as much as possible of backlog packets if any
        startRecv();
    }
}

//
// ---> handleInitRecv(const boost::system::error_code& errorCode, size_t bytesTransferred);
//
// handle packet reception
//
void LinkProber::handleInitRecv(
    const boost::system::error_code& errorCode,
    size_t bytesTransferred
)
{
    MUXLOGDEBUG(mMuxPortConfig.getPortName());

    if (errorCode != boost::asio::error::operation_aborted) {
        ether_header *ethHeader = reinterpret_cast<ether_header *> (mRxBuffer.data());
        std::array<uint8_t, ETHER_ADDR_LEN> macAddress;

        memcpy(macAddress.data(), ethHeader->ether_shost, macAddress.size());

        boost::asio::io_service::strand &strand = mLinkProberStateMachinePtr->getStrand();
        boost::asio::io_service &ioService = strand.context();
        ioService.post(strand.wrap(boost::bind(
            &LinkProberStateMachineBase::handleMackAddressUpdate,
            mLinkProberStateMachinePtr,
            macAddress
        )));
    }
}

//
// ---> handleTimeout(boost::system::error_code ec);
//
// handle ICMP packet reception timeout
//
void LinkProber::handleTimeout(boost::system::error_code errorCode)
{
    MUXLOGTRACE(boost::format("%s: server: %d, mRxSelfSeqNo: %d, mTxSeqNo: %d") %
        mMuxPortConfig.getPortName() %
        mMuxPortConfig.getServerId() %
        mRxSelfSeqNo %
        mTxSeqNo
    );

    mStream.cancel();
    if (mTxSeqNo != mRxSelfSeqNo && mTxSeqNo != mRxPeerSeqNo) {
        // post unknown event
        mLinkProberStateMachinePtr->postLinkProberStateEvent(LinkProberStateMachineBase::getIcmpUnknownEvent());
        mIcmpUnknownEventCount++;
    } 

    mIcmpPacketCount++;
    if (mIcmpPacketCount % mMuxPortConfig.getNegativeStateChangeRetryCount() == 0) {
        boost::asio::io_service::strand &strand = mLinkProberStateMachinePtr->getStrand();
        boost::asio::io_service &ioService = strand.context();
        ioService.post(strand.wrap(boost::bind(
            &LinkProberStateMachineBase::handlePckLossRatioUpdate,
            mLinkProberStateMachinePtr,
            mIcmpUnknownEventCount,
            mIcmpPacketCount
        )));
    }

    // start another cycle of send/recv
    startProbing();
}

//
// ---> handleSuspendTimeout(boost::system::error_code errorCode);
//
// handle suspend timer timeout
//
void LinkProber::handleSuspendTimeout(boost::system::error_code errorCode)
{
    MUXLOGDEBUG(mMuxPortConfig.getPortName());

    mSuspendTx = false;

    if (errorCode == boost::system::errc::success) {
        // inform the composite state machine about Suspend timer expiry
        boost::asio::io_service::strand &strand = mLinkProberStateMachinePtr->getStrand();
        boost::asio::io_service &ioService = strand.context();
        ioService.post(strand.wrap(boost::bind(
            static_cast<void (LinkProberStateMachineBase::*) (SuspendTimerExpiredEvent&)>
                (&LinkProberStateMachineBase::processEvent),
            mLinkProberStateMachinePtr,
            LinkProberStateMachineBase::getSuspendTimerExpiredEvent()
        )));
    }
}

//
// ---> startRecv();
//
// start ICMP ECHOREPLY reception
//
void LinkProber::startRecv()
{
    MUXLOGTRACE(mMuxPortConfig.getPortName());


    mStream.async_read_some(
        boost::asio::buffer(mRxBuffer, MUX_MAX_ICMP_BUFFER_SIZE),
        mStrand.wrap(boost::bind(
            &LinkProber::handleRecv,
            this,
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred
        ))
    );
}

//
// ---> startInitRecv();
//
// start ICMP ECHOREPLY reception
//
void LinkProber::startInitRecv()
{
    MUXLOGTRACE(mMuxPortConfig.getPortName());

    mStream.async_read_some(
        boost::asio::buffer(mRxBuffer, MUX_MAX_ICMP_BUFFER_SIZE),
        mStrand.wrap(boost::bind(
            &LinkProber::handleInitRecv,
            this,
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred
        ))
    );
}

//
// ---> startTimer();
//
// start ICMP ECHOREPLY timeout timer
//
void LinkProber::startTimer()
{
    MUXLOGDEBUG(mMuxPortConfig.getPortName());
    // time out these heartbeats
    mDeadlineTimer.expires_from_now(boost::posix_time::milliseconds(getProbingInterval()));
    mDeadlineTimer.async_wait(mStrand.wrap(boost::bind(
        &LinkProber::handleTimeout,
        this,
        boost::asio::placeholders::error
    )));
}

//
// ---> calculateChecksum(uint16_t *data, size_t size);
//
// calculate ICMP payload checksum
//
uint32_t LinkProber::calculateChecksum(uint16_t *data, size_t size)
{
    uint32_t sum = 0;

    while (size > 1) {
        sum += ntohs(*data++);
        size -= sizeof(uint16_t);
    }

    if (size) {
        sum += ntohs(static_cast<uint16_t> ((*reinterpret_cast<uint8_t *> (data))));
    }

    return sum;
}

//
// ---> addChecksumCarryover(uint16_t *checksum, uint32_t sum);
//
// add checksum carryover
//
void LinkProber::addChecksumCarryover(uint16_t *checksum, uint32_t sum)
{
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    *checksum = htons(~sum);
}

//
// ---> computeChecksum(icmphdr *icmpHeader, size_t size);
//
// compute ICMP checksum
//
void LinkProber::computeChecksum(icmphdr *icmpHeader, size_t size)
{
    icmpHeader->checksum = 0;
    mIcmpChecksum = calculateChecksum(
        reinterpret_cast<uint16_t *> (icmpHeader), size
    );
    addChecksumCarryover(&icmpHeader->checksum, mIcmpChecksum);
}

//
// ---> computeChecksum(iphdr *ipHeader, size_t size);
//
// compute IPv4 checksum
//
void LinkProber::computeChecksum(iphdr *ipHeader, size_t size)
{
    ipHeader->check = 0;
    mIpChecksum = calculateChecksum(
        reinterpret_cast<uint16_t *> (ipHeader), size
    );
    addChecksumCarryover(&ipHeader->check, mIpChecksum);
}

//
// ---> initializeSendBuffer();
//
// initialize ICMP packet once
//
void LinkProber::initializeSendBuffer()
{
    ether_header *ethHeader = reinterpret_cast<ether_header *> (mTxBuffer.data());
    memcpy(ethHeader->ether_dhost, mMuxPortConfig.getBladeMacAddress().data(), sizeof(ethHeader->ether_dhost));
    memcpy(ethHeader->ether_shost, mMuxPortConfig.getTorMacAddress().data(), sizeof(ethHeader->ether_shost));
    ethHeader->ether_type = htons(ETHERTYPE_IP);

    iphdr *ipHeader = reinterpret_cast<iphdr *> (mTxBuffer.data() + sizeof(ether_header));
    icmphdr *icmpHeader = reinterpret_cast<icmphdr *> (mTxBuffer.data() + sizeof(ether_header) + sizeof(iphdr));

    new (mTxBuffer.data() + mPacketHeaderSize) IcmpPayload();
    resetTxBufferTlv();
    appendTlvSentinel();
    size_t totalPayloadSize = mTxPacketSize - mPacketHeaderSize;

    ipHeader->ihl = sizeof(iphdr) >> 2;
    ipHeader->version = IPVERSION;
    ipHeader->tos = 0xb8;
    ipHeader->tot_len = htons(sizeof(iphdr) + sizeof(icmphdr) + totalPayloadSize);
    ipHeader->id = static_cast<uint16_t> (rand());
    ipHeader->frag_off = 0;
    ipHeader->ttl = 64;
    ipHeader->protocol = IPPROTO_ICMP;
    ipHeader->check = 0;
    ipHeader->saddr = htonl(mMuxPortConfig.getLoopbackIpv4Address().to_v4().to_uint());
    ipHeader->daddr = htonl(mMuxPortConfig.getBladeIpv4Address().to_v4().to_uint());
    computeChecksum(ipHeader, ipHeader->ihl << 2);

    icmpHeader->type = ICMP_ECHO;
    icmpHeader->code = 0;
    icmpHeader->un.echo.id = htons(mMuxPortConfig.getServerId());
    icmpHeader->un.echo.sequence = htons(mTxSeqNo);

    computeChecksum(icmpHeader, sizeof(icmphdr) + totalPayloadSize);
}

//
// ---> updateIcmpSequenceNo();
//
// update ICMP packet checksum, used before sending new heartbeat
//
void LinkProber::updateIcmpSequenceNo()
{
    // update received sequence to avoid reporting invalid ICMP event when sequence number rolls over
    mRxPeerSeqNo = mTxSeqNo;
    mRxSelfSeqNo = mTxSeqNo;

    icmphdr *icmpHeader = reinterpret_cast<icmphdr *> (mTxBuffer.data() + sizeof(ether_header) + sizeof(iphdr));
    icmpHeader->un.echo.sequence = htons(++mTxSeqNo);
    mIcmpChecksum += mTxSeqNo ? 1 : 0;
    addChecksumCarryover(&icmpHeader->checksum, mIcmpChecksum);
}

//
// ---> findNextTlv
//
// Find next TLV to process in rxBuffer
//
size_t LinkProber::findNextTlv(size_t readOffset, size_t bytesTransferred)
{
    size_t tlvSize = 0;
    if (readOffset + sizeof(TlvHead) <= bytesTransferred) {
        Tlv *tlvPtr = reinterpret_cast<Tlv *> (mRxBuffer.data() + readOffset);
        tlvSize = (sizeof(TlvHead) + ntohs(tlvPtr->tlvhead.length));
        if (readOffset + tlvSize > bytesTransferred) {
            tlvSize = 0;
        }
    }
    return tlvSize;
}

//
// ---> appendTlvCommand
//
// Append TlvCommand to the end of txBuffer
//
size_t LinkProber::appendTlvCommand(Command commandType)
{
    size_t tlvSize = sizeof(TlvHead) + sizeof(Command);
    assert(mTxPacketSize + tlvSize <= MUX_MAX_ICMP_BUFFER_SIZE);
    Tlv *tlvPtr = reinterpret_cast<Tlv *> (mTxBuffer.data() + mTxPacketSize);
    tlvPtr->tlvhead.type = TlvType::TLV_COMMAND;
    tlvPtr->tlvhead.length = htons(sizeof(Command));
    tlvPtr->command = static_cast<uint8_t> (commandType);
    mTxPacketSize += tlvSize;
    return tlvSize;
}

//
// ---> appendTlvSentinel
//
// Append TlvSentinel to the end of txBuffer
//
size_t LinkProber::appendTlvSentinel()
{
    size_t tlvSize = sizeof(TlvHead);
    assert(mTxPacketSize + tlvSize <= MUX_MAX_ICMP_BUFFER_SIZE);
    Tlv *tlvPtr = reinterpret_cast<Tlv *> (mTxBuffer.data() + mTxPacketSize);
    tlvPtr->tlvhead.type = TlvType::TLV_SENTINEL;
    tlvPtr->tlvhead.length = 0;
    mTxPacketSize += tlvSize;
    return tlvSize;
}

//
// ---> appendTlvDummy
//
// Append a dummy TLV, test purpose only
//
size_t LinkProber::appendTlvDummy(size_t paddingSize, int seqNo)
{
    size_t tlvSize = sizeof(TlvHead) + paddingSize + sizeof(uint32_t);
    assert(mTxPacketSize + tlvSize <= MUX_MAX_ICMP_BUFFER_SIZE);
    Tlv *tlvPtr = reinterpret_cast<Tlv *> (mTxBuffer.data() + mTxPacketSize);
    tlvPtr->tlvhead.type = TlvType::TLV_DUMMY;
    tlvPtr->tlvhead.length = htons(paddingSize + sizeof(uint32_t));
    memset(tlvPtr->data, 0, paddingSize);
    *(reinterpret_cast<uint32_t *> (tlvPtr->data + paddingSize)) = htonl(seqNo);
    mTxPacketSize += tlvSize;
    return tlvSize;
}

// 
// ---> resetIcmpPacketCounts
//
// reset Icmp packet counts, post a pck loss ratio update immediately 
//
void LinkProber::resetIcmpPacketCounts()
{
    mIcmpUnknownEventCount = 0;
    mIcmpPacketCount = 0;

    boost::asio::io_service::strand &strand = mLinkProberStateMachinePtr->getStrand();
    boost::asio::io_service &ioService = strand.context();
    ioService.post(strand.wrap(boost::bind(
        &LinkProberStateMachineBase::handlePckLossRatioUpdate,
        mLinkProberStateMachinePtr,
        mIcmpUnknownEventCount,
        mIcmpPacketCount
    )));
}

//
// ---> decreaseProbingInter
//
//  adjust link prober interval to 10 ms after switchover to better measure the switchover overhead.
//
void LinkProber::decreaseProbingIntervalAfterSwitch()
{
    mDecreaseProbingInterval =  true;
}

//
// ---> revertProbingIntervalChangeAfterSwitch
// 
// revert probing interval change after switch 
//
void LinkProber::revertProbingIntervalChangeAfterSwitch()
{
    mDecreaseProbingInterval = false;
}

//
// ---> getProbingInterval
// 
// get link prober interval
//
uint32_t LinkProber::getProbingInterval()
{
    return mDecreaseProbingInterval? 10:mMuxPortConfig.getTimeoutIpv4_msec();
}

} /* namespace link_prober */
