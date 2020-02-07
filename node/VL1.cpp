/*
 * Copyright (c)2013-2020 ZeroTier, Inc.
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file in the project's root directory.
 *
 * Change Date: 2024-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2.0 of the Apache License.
 */
/****/

#include "VL1.hpp"
#include "RuntimeEnvironment.hpp"
#include "Node.hpp"
#include "Topology.hpp"
#include "VL2.hpp"
#include "Salsa20.hpp"
#include "LZ4.hpp"
#include "Poly1305.hpp"
#include "Identity.hpp"
#include "SelfAwareness.hpp"
#include "SHA512.hpp"
#include "Peer.hpp"
#include "Path.hpp"

namespace ZeroTier {

namespace {

ZT_ALWAYS_INLINE const Identity &ifPeerNonNull(const SharedPtr<Peer> &p)
{
	if (p)
		return p->identity();
	return Identity::NIL;
}

} // anonymous namespace

VL1::VL1(const RuntimeEnvironment *renv) :
	RR(renv)
{
}

VL1::~VL1()
{
}

void VL1::onRemotePacket(void *const tPtr,const int64_t localSocket,const InetAddress &fromAddr,SharedPtr<Buf> &data,const unsigned int len)
{
	const int64_t now = RR->node->now();
	const SharedPtr<Path> path(RR->topology->getPath(localSocket,fromAddr));
	path->received(now);

	// Really short packets are keepalives and other junk.
	if (len < ZT_PROTO_MIN_FRAGMENT_LENGTH)
		return;

	try {
		FCV<Buf::Slice,ZT_MAX_PACKET_FRAGMENTS> pktv;
		Address destination;

		if (data->b[ZT_PROTO_PACKET_FRAGMENT_INDICATOR_INDEX] == ZT_PROTO_PACKET_FRAGMENT_INDICATOR) {
			// Fragment -----------------------------------------------------------------------------------------------------

			const Protocol::FragmentHeader &fh = data->as<Protocol::FragmentHeader>();
			destination.setTo(fh.destination);

			if (destination != RR->identity.address()) {
				// Fragment is not address to this node -----------------------------------------------------------------------
				_relay(tPtr,path,destination,data,len);
				return;
			}

			switch (_inputPacketAssembler.assemble(
				fh.packetId,
				pktv,
				data,
				ZT_PROTO_PACKET_FRAGMENT_PAYLOAD_START_AT,
				(unsigned int)(len - ZT_PROTO_PACKET_FRAGMENT_PAYLOAD_START_AT),
				fh.counts & 0xfU, // fragment number
				fh.counts >> 4U,  // total number of fragments in message is specified in each fragment
				now,
				path,
				ZT_MAX_INCOMING_FRAGMENTS_PER_PATH)) {
				case Defragmenter<ZT_MAX_PACKET_FRAGMENTS>::COMPLETE:
					break;
				default:
					//case Defragmenter<ZT_MAX_PACKET_FRAGMENTS>::OK:
					//case Defragmenter<ZT_MAX_PACKET_FRAGMENTS>::ERR_DUPLICATE_FRAGMENT:
					//case Defragmenter<ZT_MAX_PACKET_FRAGMENTS>::ERR_INVALID_FRAGMENT:
					//case Defragmenter<ZT_MAX_PACKET_FRAGMENTS>::ERR_TOO_MANY_FRAGMENTS_FOR_PATH:
					//case Defragmenter<ZT_MAX_PACKET_FRAGMENTS>::ERR_OUT_OF_MEMORY:
					return;
			}
		} else {
			// Not fragment, meaning whole packet or head of series with fragments ------------------------------------------

			if (len < ZT_PROTO_MIN_PACKET_LENGTH)
				return;
			const Protocol::Header &ph = data->as<Protocol::Header>();
			destination.setTo(ph.destination);

			if (destination != RR->identity.address()) {
				// Packet or packet head is not address to this node ----------------------------------------------------------
				_relay(tPtr,path,destination,data,len);
				return;
			}

			if ((ph.flags & ZT_PROTO_FLAG_FRAGMENTED) != 0) {
				// Head of fragmented packet ----------------------------------------------------------------------------------
				switch (_inputPacketAssembler.assemble(
					ph.packetId,
					pktv,
					data,
					0,
					len,
					0, // always the zero'eth fragment
					0, // this is specified in fragments, not in the head
					now,
					path,
					ZT_MAX_INCOMING_FRAGMENTS_PER_PATH)) {
					case Defragmenter<ZT_MAX_PACKET_FRAGMENTS>::COMPLETE:
						break;
					default:
						//case Defragmenter<ZT_MAX_PACKET_FRAGMENTS>::OK:
						//case Defragmenter<ZT_MAX_PACKET_FRAGMENTS>::ERR_DUPLICATE_FRAGMENT:
						//case Defragmenter<ZT_MAX_PACKET_FRAGMENTS>::ERR_INVALID_FRAGMENT:
						//case Defragmenter<ZT_MAX_PACKET_FRAGMENTS>::ERR_TOO_MANY_FRAGMENTS_FOR_PATH:
						//case Defragmenter<ZT_MAX_PACKET_FRAGMENTS>::ERR_OUT_OF_MEMORY:
						return;
				}
			} else {
				// Unfragmented packet, skip defrag engine and just handle it -------------------------------------------------
				Buf::Slice &s = pktv.push();
				s.b = data;
				s.s = 0;
				s.e = len;
			}
		}

		// Packet defragmented and apparently addressed to this node ------------------------------------------------------

		// Subject pktv to a few sanity checks just to make sure Defragmenter worked correctly and
		// there is enough room in each slice to shift their contents to sizes that are multiples
		// of 64 if needed for crypto.
		if ((pktv.empty()) || (((int)pktv[0].e - (int)pktv[0].s) < sizeof(Protocol::Header))) {
			RR->t->unexpectedError(tPtr,0x3df19990,"empty or undersized packet vector");
			return;
		}
		for(FCV<Buf::Slice,ZT_MAX_PACKET_FRAGMENTS>::const_iterator s(pktv.begin());s!=pktv.end();++s) {
			if ((s->e > (ZT_BUF_MEM_SIZE - 64))||(s->s > s->e))
				return;
		}

		Protocol::Header *ph = &(pktv[0].b->as<Protocol::Header>(pktv[0].s));
		const Address source(ph->source);

		if (source == RR->identity.address())
			return;
		SharedPtr<Peer> peer(RR->topology->get(tPtr,source));

		Buf::Slice pkt;
		bool authenticated = false;

		const uint8_t hops = Protocol::packetHops(*ph);
		const uint8_t cipher = Protocol::packetCipher(*ph);

		unsigned int packetSize = pktv[0].e - pktv[0].s;
		for(FCV<Buf::Slice,ZT_MAX_PACKET_FRAGMENTS>::const_iterator s(pktv.begin()+1);s!=pktv.end();++s)
			packetSize += s->e - s->s;
		if (packetSize > ZT_PROTO_MAX_PACKET_LENGTH) {
			RR->t->incomingPacketDropped(tPtr,0x010348da,ph->packetId,0,ifPeerNonNull(peer),path->address(),hops,Protocol::VERB_NOP,ZT_TRACE_PACKET_DROP_REASON_MALFORMED_PACKET);
			return;
		}

		// If we don't know this peer and this is not a HELLO, issue a WHOIS and enqueue this packet to try again.
		if ((!peer)&&(!(((cipher == ZT_PROTO_CIPHER_SUITE__POLY1305_NONE)||(cipher == ZT_PROTO_CIPHER_SUITE__NONE))&&((ph->verb & 0x1fU) == Protocol::VERB_HELLO)))) {
			pkt = Buf::assembleSliceVector(pktv);
			if (pkt.e < ZT_PROTO_MIN_PACKET_LENGTH) {
				RR->t->incomingPacketDropped(tPtr,0xbada9366,ph->packetId,0,ifPeerNonNull(peer),path->address(),hops,Protocol::VERB_NOP,ZT_TRACE_PACKET_DROP_REASON_MALFORMED_PACKET);
				return;
			}
			{
				Mutex::Lock wl(_whoisQueue_l);
				_WhoisQueueItem &wq = _whoisQueue[source];
				wq.inboundPackets.push_back(pkt);
			}
			_sendPendingWhois(tPtr,now);
			return;
		}

		switch(cipher) {
			case ZT_PROTO_CIPHER_SUITE__POLY1305_NONE:
				if (peer) {
					pkt = Buf::assembleSliceVector(pktv);
					if (pkt.e < ZT_PROTO_MIN_PACKET_LENGTH) {
						RR->t->incomingPacketDropped(tPtr,0x432aa9da,ph->packetId,0,peer->identity(),path->address(),hops,Protocol::VERB_NOP,ZT_TRACE_PACKET_DROP_REASON_MALFORMED_PACKET);
						return;
					}
					ph = &(pkt.b->as<Protocol::Header>());

					// Generate one-time-use MAC key using Salsa20.
					uint8_t perPacketKey[ZT_PEER_SECRET_KEY_LENGTH];
					uint8_t macKey[ZT_POLY1305_KEY_LEN];
					Protocol::salsa2012DeriveKey(peer->key(),perPacketKey,*pktv[0].b,packetSize);
					Salsa20(perPacketKey,&ph->packetId).crypt12(Utils::ZERO256,macKey,ZT_POLY1305_KEY_LEN);

					// Verify packet MAC.
					uint64_t mac[2];
					poly1305(mac,pkt.b->b + ZT_PROTO_PACKET_ENCRYPTED_SECTION_START,packetSize - ZT_PROTO_PACKET_ENCRYPTED_SECTION_START,macKey);
					if (ph->mac != mac[0]) {
						RR->t->incomingPacketDropped(tPtr,0xcc89c812,ph->packetId,0,peer->identity(),path->address(),hops,Protocol::VERB_NOP,ZT_TRACE_PACKET_DROP_REASON_MAC_FAILED);
						return;
					}
					authenticated = true;
				}
				break;

			case ZT_PROTO_CIPHER_SUITE__POLY1305_SALSA2012:
				if (peer) {
					// Derive per-packet key using symmetric key plus some data from the packet header.
					uint8_t perPacketKey[ZT_PEER_SECRET_KEY_LENGTH];
					Protocol::salsa2012DeriveKey(peer->key(),perPacketKey,*pktv[0].b,packetSize);
					Salsa20 s20(perPacketKey,&ph->packetId);

					// Do one Salsa20 block to generate the one-time-use Poly1305 key.
					uint8_t macKey[ZT_POLY1305_KEY_LEN];
					s20.crypt12(Utils::ZERO256,macKey,ZT_POLY1305_KEY_LEN);

					// Get a buffer to store the decrypted and fully contiguous packet.
					pkt.b = Buf::get();
					if (!pkt.b) {
						RR->t->unexpectedError(tPtr,0x1de16991,"Buf::get() failed (out of memory?)");
						return;
					}

					// Salsa20 is a stream cipher but it's only seekable to multiples of 64 bytes.
					// This moves data in slices around so that all slices have sizes that are
					// multiples of 64 except the last slice. Note that this does not corrupt
					// the assembled slice vector, just moves data around.
					if (pktv.size() > 1) {
						unsigned int prevOverflow,i;
						for (typename FCV<Buf::Slice,ZT_MAX_PACKET_FRAGMENTS>::iterator ps(pktv.begin()),s(ps + 1);s!=pktv.end();) {
							prevOverflow = (ps->e - ps->s) & 63U; // amount by which previous exceeds a multiple of 64
							for(i=0;i<prevOverflow;++i) {
								if (s->s >= s->e)
									goto next_slice;
								ps->b->b[ps->e++] = s->b->b[s->s++]; // move from head of current to end of previous
							}
							next_slice: ps = s++;
						}
					}

					// Simultaneously decrypt and assemble packet into a contiguous buffer.
					// Since we moved data around above all slices will have sizes that are
					// multiples of 64.
					memcpy(pkt.b->b,ph,sizeof(Protocol::Header));
					pkt.e = sizeof(Protocol::Header);
					for(FCV<Buf::Slice,ZT_MAX_PACKET_FRAGMENTS>::iterator s(pktv.begin());s!=pktv.end();++s) {
						const unsigned int sliceSize = s->e - s->s;
						s20.crypt12(s->b->b + s->s,pkt.b->b + pkt.e,sliceSize);
						pkt.e += sliceSize;
					}
					ph = &(pkt.b->as<Protocol::Header>());

					// Verify packet MAC.
					uint64_t mac[2];
					poly1305(mac,pkt.b->b + ZT_PROTO_PACKET_ENCRYPTED_SECTION_START,packetSize - ZT_PROTO_PACKET_ENCRYPTED_SECTION_START,macKey);
					if (ph->mac != mac[0]) {
						RR->t->incomingPacketDropped(tPtr,0xbc881231,ph->packetId,0,peer->identity(),path->address(),hops,Protocol::VERB_NOP,ZT_TRACE_PACKET_DROP_REASON_MAC_FAILED);
						return;
					}
					authenticated = true;
				} else {
					RR->t->incomingPacketDropped(tPtr,0xb0b01999,ph->packetId,0,ifPeerNonNull(peer),path->address(),hops,Protocol::VERB_NOP,ZT_TRACE_PACKET_DROP_REASON_MAC_FAILED);
					return;
				}
				break;

			case ZT_PROTO_CIPHER_SUITE__NONE: {
				// CIPHER_SUITE__NONE is only used with trusted paths. Verification is performed by
				// checking the address and the presence of its corresponding trusted path ID in the
				// packet header's MAC field.

				pkt = Buf::assembleSliceVector(pktv);
				if (pkt.e < ZT_PROTO_MIN_PACKET_LENGTH)
					RR->t->incomingPacketDropped(tPtr,0x3d3337df,ph->packetId,0,ifPeerNonNull(peer),path->address(),hops,Protocol::VERB_NOP,ZT_TRACE_PACKET_DROP_REASON_MALFORMED_PACKET);
				ph = &(pkt.b->as<Protocol::Header>());

				if (RR->topology->shouldInboundPathBeTrusted(path->address(),Utils::ntoh(ph->mac))) {
					authenticated = true;
				} else {
					RR->t->incomingPacketDropped(tPtr,0x2dfa910b,ph->packetId,0,ifPeerNonNull(peer),path->address(),hops,Protocol::VERB_NOP,ZT_TRACE_PACKET_DROP_REASON_NOT_TRUSTED_PATH);
					return;
				}
			} break;

			//case ZT_PROTO_CIPHER_SUITE__AES_GCM_NRH:
			//	if (peer) {
			//	}
			//	break;

			default:
				RR->t->incomingPacketDropped(tPtr,0x5b001099,ph->packetId,0,ifPeerNonNull(peer),path->address(),hops,Protocol::VERB_NOP,ZT_TRACE_PACKET_DROP_REASON_INVALID_OBJECT);
				return;
		}

		// Packet fully assembled and may be authenticated ----------------------------------------------------------------

		// Return any still held buffers in pktv to the buffer pool.
		pktv.clear();

		const Protocol::Verb verb = (Protocol::Verb)(ph->verb & ZT_PROTO_VERB_MASK);

		// Decompress packet payload if compressed.
		if ((ph->verb & ZT_PROTO_VERB_FLAG_COMPRESSED) != 0) {
			if (!authenticated) {
				RR->t->incomingPacketDropped(tPtr,0x390bcd0a,ph->packetId,0,ifPeerNonNull(peer),path->address(),hops,verb,ZT_TRACE_PACKET_DROP_REASON_MALFORMED_PACKET);
				return;
			}

			SharedPtr<Buf> nb(Buf::get());
			if (!nb) {
				RR->t->unexpectedError(tPtr,0xffe169fa,"Buf::get() failed (out of memory?)");
				return;
			}

			const int uncompressedLen = LZ4_decompress_safe(
				reinterpret_cast<const char *>(pkt.b->b + ZT_PROTO_PACKET_PAYLOAD_START),
				reinterpret_cast<char *>(nb->b),
				(int)(packetSize - ZT_PROTO_PACKET_PAYLOAD_START),
				ZT_BUF_MEM_SIZE - ZT_PROTO_PACKET_PAYLOAD_START);

			if ((uncompressedLen > 0)&&(uncompressedLen <= (ZT_BUF_MEM_SIZE - ZT_PROTO_PACKET_PAYLOAD_START))) {
				pkt.b.swap(nb);
				pkt.e = packetSize = (unsigned int)uncompressedLen;
			} else {
				RR->t->incomingPacketDropped(tPtr,0xee9e4392,ph->packetId,0,ifPeerNonNull(peer),path->address(),hops,verb,ZT_TRACE_PACKET_DROP_REASON_INVALID_COMPRESSED_DATA);
				return;
			}
		}

		// VL1 and VL2 are conceptually and (mostly) logically separate layers.
		// Verbs that relate to VL1 (P2P transport) are handled in this class.
		// VL2 (virtual Ethernet / SDN) verbs are handled in the VL2 class.
		switch(verb) {
			case Protocol::VERB_NOP:
				peer->received(tPtr,path,hops,ph->packetId,packetSize - ZT_PROTO_PACKET_PAYLOAD_START,Protocol::VERB_NOP,0,Protocol::VERB_NOP,0);
				break;

			case Protocol::VERB_HELLO:                      _HELLO(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_ERROR:                      _ERROR(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_OK:                         _OK(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_WHOIS:                      _WHOIS(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_RENDEZVOUS:                 _RENDEZVOUS(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_FRAME:                      RR->vl2->_FRAME(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_EXT_FRAME:                  RR->vl2->_EXT_FRAME(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_ECHO:                       _ECHO(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated);
			case Protocol::VERB_MULTICAST_LIKE:             RR->vl2->_MULTICAST_LIKE(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_NETWORK_CREDENTIALS:        RR->vl2->_NETWORK_CREDENTIALS(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_NETWORK_CONFIG_REQUEST:     RR->vl2->_NETWORK_CONFIG_REQUEST(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_NETWORK_CONFIG:             RR->vl2->_NETWORK_CONFIG(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_MULTICAST_GATHER:           RR->vl2->_MULTICAST_GATHER(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_MULTICAST_FRAME_deprecated: RR->vl2->_MULTICAST_FRAME_deprecated(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_PUSH_DIRECT_PATHS:          _PUSH_DIRECT_PATHS(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_USER_MESSAGE:               _USER_MESSAGE(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_MULTICAST:                  RR->vl2->_MULTICAST(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;
			case Protocol::VERB_ENCAP:                      _ENCAP(tPtr,path,peer,*pkt.b,(int)packetSize,authenticated); break;

			default:
				RR->t->incomingPacketDropped(tPtr,0xdeadeff0,ph->packetId,0,ifPeerNonNull(peer),path->address(),hops,verb,ZT_TRACE_PACKET_DROP_REASON_UNRECOGNIZED_VERB);
				break;
		}
	} catch ( ... ) {
		RR->t->unexpectedError(tPtr,0xea1b6dea,"unexpected exception in onRemotePacket()");
	}
}

void VL1::_relay(void *tPtr,const SharedPtr<Path> &path,const Address &destination,SharedPtr<Buf> &data,unsigned int len)
{
}

void VL1::_sendPendingWhois(void *const tPtr,const int64_t now)
{
	SharedPtr<Peer> root(RR->topology->root());
	if (!root)
		return;
	SharedPtr<Path> rootPath(root->path(now));
	if (!rootPath)
		return;

	std::vector<Address> toSend;
	{
		Mutex::Lock wl(_whoisQueue_l);
		Hashtable<Address,_WhoisQueueItem>::Iterator wi(_whoisQueue);
		Address *a = nullptr;
		_WhoisQueueItem *wq = nullptr;
		while (wi.next(a,wq)) {
			if ((now - wq->lastRetry) >= ZT_WHOIS_RETRY_DELAY) {
				wq->lastRetry = now;
				++wq->retries;
				toSend.push_back(*a);
			}
		}
	}

	Buf outp;
	Protocol::Header &ph = outp.as<Protocol::Header>();

	std::vector<Address>::iterator a(toSend.begin());
	while (a != toSend.end()) {
		ph.packetId = Protocol::getPacketId();
		root->address().copyTo(ph.destination);
		RR->identity.address().copyTo(ph.source);
		ph.flags = 0;
		ph.verb = Protocol::VERB_OK;

		int ptr = sizeof(Protocol::Header);
		while ((a != toSend.end())&&(ptr < (ZT_PROTO_MAX_PACKET_LENGTH - 1))) {
			a->copyTo(outp.b + ptr);
			++a;
			ptr += ZT_ADDRESS_LENGTH;
		}

		if (ptr > sizeof(Protocol::Header)) {
			Protocol::armor(outp,ptr,root->key(),ZT_PROTO_CIPHER_SUITE__POLY1305_SALSA2012);
			rootPath->send(RR,tPtr,outp.b,ptr,now);
		}
	}
}

void VL1::_HELLO(void *tPtr,const SharedPtr<Path> &path,SharedPtr<Peer> &peer,Buf &pkt,int packetSize,bool authenticated)
{
	if (packetSize < sizeof(Protocol::HELLO)) {
		RR->t->incomingPacketDropped(tPtr,0x2bdb0001,0,0,ifPeerNonNull(peer),path->address(),0,Protocol::VERB_HELLO,ZT_TRACE_PACKET_DROP_REASON_MALFORMED_PACKET);
		return;
	}

	Protocol::HELLO &p = pkt.as<Protocol::HELLO>();
	const uint8_t hops = Protocol::packetHops(p.h);
	int ptr = sizeof(Protocol::HELLO);

	if (p.versionProtocol < ZT_PROTO_VERSION_MIN) {
		RR->t->incomingPacketDropped(tPtr,0xe8d12bad,p.h.packetId,0,ifPeerNonNull(peer),path->address(),hops,Protocol::VERB_HELLO,ZT_TRACE_PACKET_DROP_REASON_PEER_TOO_OLD);
		return;
	}

	Identity id;
	if (pkt.rO(ptr,id) < 0) {
		RR->t->incomingPacketDropped(tPtr,0x707a9810,p.h.packetId,0,ifPeerNonNull(peer),path->address(),hops,Protocol::VERB_HELLO,ZT_TRACE_PACKET_DROP_REASON_INVALID_OBJECT);
		return;
	}
	if (Address(p.h.source) != id.address()) {
		RR->t->incomingPacketDropped(tPtr,0x06aa9ff1,p.h.packetId,0,Identity::NIL,path->address(),hops,Protocol::VERB_HELLO,ZT_TRACE_PACKET_DROP_REASON_MAC_FAILED);
		return;
	}

	// Packet is basically valid and identity unmarshaled ---------------------------------------------------------------

	// Get long-term static key for this node.
	uint8_t key[ZT_PEER_SECRET_KEY_LENGTH];
	if ((peer)&&(id == peer->identity())) {
		memcpy(key,peer->key(),ZT_PEER_SECRET_KEY_LENGTH);
	} else {
		peer.zero();
		if (!RR->identity.agree(id,key)) {
			RR->t->incomingPacketDropped(tPtr,0x46db8010,p.h.packetId,0,id,path->address(),hops,Protocol::VERB_HELLO,ZT_TRACE_PACKET_DROP_REASON_MAC_FAILED);
			return;
		}
	}

	// Verify packet using Poly1305, which for v2.x
	{
		uint8_t perPacketKey[ZT_PEER_SECRET_KEY_LENGTH];
		uint8_t macKey[ZT_POLY1305_KEY_LEN];
		Protocol::salsa2012DeriveKey(peer->key(),perPacketKey,pkt,packetSize);
		Salsa20(perPacketKey,&p.h.packetId).crypt12(Utils::ZERO256,macKey,ZT_POLY1305_KEY_LEN);
		uint64_t mac[2];
		poly1305(mac,pkt.b + ZT_PROTO_PACKET_ENCRYPTED_SECTION_START,packetSize - ZT_PROTO_PACKET_ENCRYPTED_SECTION_START,macKey);
		if (p.h.mac != mac[0]) {
			RR->t->incomingPacketDropped(tPtr,0x11bfff81,p.h.packetId,0,id,path->address(),hops,Protocol::VERB_NOP,ZT_TRACE_PACKET_DROP_REASON_MAC_FAILED);
			return;
		}
	}

	// Packet has passed Poly1305 verification --------------------------------------------------------------------------

	InetAddress externalSurfaceAddress;
	Dictionary nodeMetaData;
	uint8_t hmacKey[ZT_PEER_SECRET_KEY_LENGTH],hmac[ZT_HMACSHA384_LEN];
	bool hmacAuthenticated = false;

	// Get external surface address if present.
	if (ptr < packetSize) {
		if (pkt.rO(ptr,externalSurfaceAddress) < 0) {
			RR->t->incomingPacketDropped(tPtr,0xf1000023,p.h.packetId,0,id,path->address(),hops,Protocol::VERB_HELLO,ZT_TRACE_PACKET_DROP_REASON_INVALID_OBJECT);
			return;
		}
	}

	if (ptr < packetSize) {
		// Everything after this point is encrypted with Salsa20/12. This is only a privacy measure
		// since there's nothing truly secret in a HELLO packet. It also means that an observer
		// can't even get ephemeral public keys without first knowing the long term secret key,
		// adding a little defense in depth.
		uint8_t iv[8];
		for(int i=0;i<8;++i) iv[i] = pkt.b[i];
		iv[7] &= 0xf8U;
		Salsa20 s20(key,iv);
		s20.crypt12(pkt.b + ptr,pkt.b + ptr,packetSize - ptr);

		ptr += pkt.rI16(ptr); // this field is zero in v2.0+ but can indicate data between this point and dictionary
		if (ptr < packetSize) {
			const unsigned int dictionarySize = pkt.rI16(ptr);
			const void *const dictionaryBytes = pkt.b + ptr;
			if ((ptr += (int)dictionarySize) > packetSize) {
				RR->t->incomingPacketDropped(tPtr,0x0d0f0112,p.h.packetId,0,id,path->address(),hops,Protocol::VERB_HELLO,ZT_TRACE_PACKET_DROP_REASON_INVALID_OBJECT);
				return;
			}

			ptr += pkt.rI16(ptr); // skip any additional fields, currently always 0
			if (ptr > packetSize) {
				RR->t->incomingPacketDropped(tPtr,0x451f2341,0,p.h.packetId,id,path->address(),0,Protocol::VERB_HELLO,ZT_TRACE_PACKET_DROP_REASON_MALFORMED_PACKET);
				return;
			}

			if ((ptr + ZT_SHA384_DIGEST_LEN) <= packetSize) {
				KBKDFHMACSHA384(key,ZT_PROTO_KDF_KEY_LABEL_HELLO_HMAC,0,0,hmacKey); // iter == 0 for HELLO
				HMACSHA384(hmacKey,pkt.b + ZT_PROTO_PACKET_ENCRYPTED_SECTION_START,packetSize - ZT_PROTO_PACKET_ENCRYPTED_SECTION_START,hmac);
				if (!Utils::secureEq(pkt.b + ptr,hmac,ZT_HMACSHA384_LEN)) {
					RR->t->incomingPacketDropped(tPtr,0x1000662a,p.h.packetId,0,id,path->address(),hops,Protocol::VERB_NOP,ZT_TRACE_PACKET_DROP_REASON_MAC_FAILED);
					return;
				}
				hmacAuthenticated = true;
			}

			if (dictionarySize) {
				if (!nodeMetaData.decode(dictionaryBytes,dictionarySize)) {
					RR->t->incomingPacketDropped(tPtr,0x67192344,p.h.packetId,0,id,path->address(),hops,Protocol::VERB_HELLO,ZT_TRACE_PACKET_DROP_REASON_INVALID_OBJECT);
					return;
				}
			}
		}
	}

	// v2.x+ peers must include HMAC, older peers don't
	if ((!hmacAuthenticated)&&(p.versionProtocol >= 11)) {
		RR->t->incomingPacketDropped(tPtr,0x571feeea,p.h.packetId,0,id,path->address(),hops,Protocol::VERB_NOP,ZT_TRACE_PACKET_DROP_REASON_MAC_FAILED);
		return;
	}

	// Packet is fully decoded and has passed full HMAC (if present) ----------------------------------------------------

	const int64_t now = RR->node->now();

	if (!peer) {
		if (!RR->node->rateGateIdentityVerification(now,path->address())) {
			RR->t->incomingPacketDropped(tPtr,0xaffa9ff7,p.h.packetId,0,id,path->address(),hops,Protocol::VERB_HELLO,ZT_TRACE_PACKET_DROP_REASON_RATE_LIMIT_EXCEEDED);
			return;
		}
		if (!id.locallyValidate()) {
			RR->t->incomingPacketDropped(tPtr,0x2ff7a909,p.h.packetId,0,id,path->address(),hops,Protocol::VERB_HELLO,ZT_TRACE_PACKET_DROP_REASON_INVALID_OBJECT);
			return;
		}
		peer.set(new Peer(RR));
		if (!peer)
			return;
		peer->init(RR->identity,id);
		peer = RR->topology->add(tPtr,peer);
	}

	// All validation steps complete, peer learned if not yet known -----------------------------------------------------

	if ((hops == 0)&&(externalSurfaceAddress))
		RR->sa->iam(tPtr,id,path->localSocket(),path->address(),externalSurfaceAddress,RR->topology->isRoot(id),now);

	std::vector<uint8_t> myNodeMetaDataBin;
	{
		Dictionary myNodeMetaData;
		myNodeMetaData.encode(myNodeMetaDataBin);
	}
	if (myNodeMetaDataBin.size() > ZT_PROTO_MAX_PACKET_LENGTH) // sanity check
		return;

	Buf outp;
	Protocol::OK::HELLO &ok = outp.as<Protocol::OK::HELLO>();

	ok.h.h.packetId = Protocol::getPacketId();
	id.address().copyTo(ok.h.h.destination);
	RR->identity.address().copyTo(ok.h.h.source);
	ok.h.h.flags = 0;
	ok.h.h.verb = Protocol::VERB_OK;

	ok.h.inReVerb = Protocol::VERB_HELLO;
	ok.h.inRePacketId = p.h.packetId;

	ok.timestampEcho = p.timestamp;
	ok.versionProtocol = ZT_PROTO_VERSION;
	ok.versionMajor = ZEROTIER_ONE_VERSION_MAJOR;
	ok.versionMinor = ZEROTIER_ONE_VERSION_MINOR;
	ok.versionRev = ZT_CONST_TO_BE_UINT16(ZEROTIER_ONE_VERSION_REVISION);

	int outl = sizeof(Protocol::OK::HELLO);
	outp.wO(outl,path->address());

	if (p.versionProtocol >= 11) {
		outp.wI(outl,(uint16_t)0); // legacy field, always 0
		outp.wI(outl,(uint16_t)myNodeMetaDataBin.size());
		outp.wB(outl,myNodeMetaDataBin.data(),(unsigned int)myNodeMetaDataBin.size());
		outp.wI(outl,(uint16_t)0); // length of additional fields, currently 0

		if ((outl + ZT_HMACSHA384_LEN) > ZT_PROTO_MAX_PACKET_LENGTH) // sanity check, shouldn't be possible
			return;

		KBKDFHMACSHA384(key,ZT_PROTO_KDF_KEY_LABEL_HELLO_HMAC,0,1,hmacKey); // iter == 1 for OK
		HMACSHA384(hmacKey,outp.b + sizeof(ok.h),outl - sizeof(ok.h),outp.b + outl);
		outl += ZT_HMACSHA384_LEN;
	}

	Protocol::armor(outp,outl,peer->key(),ZT_PROTO_CIPHER_SUITE__POLY1305_SALSA2012);
	path->send(RR,tPtr,outp.b,outl,now);

	peer->setRemoteVersion(p.versionProtocol,p.versionMajor,p.versionMinor,Utils::ntoh(p.versionRev));
	peer->received(tPtr,path,hops,p.h.packetId,packetSize - ZT_PROTO_PACKET_PAYLOAD_START,Protocol::VERB_HELLO,0,Protocol::VERB_NOP,0);
}

void VL1::_ERROR(void *tPtr,const SharedPtr<Path> &path,const SharedPtr<Peer> &peer,Buf &pkt,int packetSize,bool authenticated)
{
}

void VL1::_OK(void *tPtr,const SharedPtr<Path> &path,const SharedPtr<Peer> &peer,Buf &pkt,int packetSize,bool authenticated)
{
}

void VL1::_WHOIS(void *tPtr,const SharedPtr<Path> &path,const SharedPtr<Peer> &peer,Buf &pkt,int packetSize,bool authenticated)
{
}

void VL1::_RENDEZVOUS(void *tPtr,const SharedPtr<Path> &path,const SharedPtr<Peer> &peer,Buf &pkt,int packetSize,bool authenticated)
{
}

void VL1::_ECHO(void *tPtr,const SharedPtr<Path> &path,const SharedPtr<Peer> &peer,Buf &pkt,int packetSize,bool authenticated)
{
}

void VL1::_PUSH_DIRECT_PATHS(void *tPtr,const SharedPtr<Path> &path,const SharedPtr<Peer> &peer,Buf &pkt,int packetSize,bool authenticated)
{
}

void VL1::_USER_MESSAGE(void *tPtr,const SharedPtr<Path> &path,const SharedPtr<Peer> &peer,Buf &pkt,int packetSize,bool authenticated)
{
}

void VL1::_ENCAP(void *tPtr,const SharedPtr<Path> &path,const SharedPtr<Peer> &peer,Buf &pkt,int packetSize,bool authenticated)
{
}

} // namespace ZeroTier
