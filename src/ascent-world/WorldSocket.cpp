/*
 * Ascent MMORPG Server
 * Copyright (C) 2005-2008 Ascent Team <http://www.ascentemu.com/>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
// Class WorldSocket - Main network code functions, handles
// reading/writing of all packets.

#include "StdAfx.h"
#include "AuthCodes.h"

/* echo send/received packets to console */
//#define ECHO_PACKET_LOG_TO_CONSOLE 1

#ifndef CLUSTERING
#pragma pack(push, 1)
struct ClientPktHeader
{
	uint16 size;
	uint32 cmd;
};

struct ServerPktHeader
{
	uint16 size;
	uint16 cmd;
};
#pragma pack(pop)

WorldSocket::WorldSocket(SOCKET fd) : Socket(fd, sWorld.SocketSendBufSize, sWorld.SocketRecvBufSize)
{
	Authed = false;
	mSize = mOpcode = mRemaining = 0;
	_latency = 0;
	mSession = NULL;
	mSeed = rand() % 0xFFFFFFF0 + 10;
	pAuthenticationPacket = NULL;
	mQueued = false;
	mRequestID = 0;
	m_nagleEanbled = false;
	m_fullAccountName = NULL;
}

WorldSocket::~WorldSocket()
{
	WorldPacket * pck;
	while((pck = _queue.Pop()))
		delete pck;

	if(pAuthenticationPacket)
		delete pAuthenticationPacket;

	if(mSession)
	{
		mSession->SetSocket(NULL);
		mSession=NULL;
	}

	if( m_fullAccountName != NULL )
	{
		delete m_fullAccountName;
		m_fullAccountName = NULL;
	}
}

void WorldSocket::OnDisconnect()
{
	if(mSession)
	{
		mSession->SetSocket(0);
		mSession=NULL;
	}

	if(mRequestID != 0)
	{
		sLogonCommHandler.UnauthedSocketClose(mRequestID);
		mRequestID = 0;
	}

	if(mQueued)
	{
		sWorld.RemoveQueuedSocket(this);	// Remove from queued sockets.
		mQueued=false;
	}
}

void WorldSocket::OutPacket(uint16 opcode, size_t len, const void* data)
{
	OUTPACKET_RESULT res;
	if( (len + 10) > WORLDSOCKET_SENDBUF_SIZE )
	{
		printf("WARNING: Tried to send a packet of %u bytes (which is too large) to a socket. Opcode was: %u (0x%03X)\n", (unsigned int)len, (unsigned int)opcode, (unsigned int)opcode);
		return;
	}

	res = _OutPacket(opcode, len, data);
	if(res == OUTPACKET_RESULT_SUCCESS)
		return;

	if(res == OUTPACKET_RESULT_NO_ROOM_IN_BUFFER)
	{
		/* queue the packet */
		queueLock.Acquire();
		WorldPacket * pck = new WorldPacket(opcode, len);
		if(len) pck->append((const uint8*)data, len);
		_queue.Push(pck);
		queueLock.Release();
	}
}

void WorldSocket::UpdateQueuedPackets()
{
	queueLock.Acquire();
	if(!_queue.HasItems())
	{
		queueLock.Release();
		return;
	}

	WorldPacket * pck;
	while((pck = _queue.front()))
	{
		/* try to push out as many as you can */
		switch(_OutPacket(pck->GetOpcode(), pck->size(), pck->size() ? pck->contents() : NULL))
		{
		case OUTPACKET_RESULT_SUCCESS:
			{
				delete pck;
				_queue.pop_front();
			}break;

		case OUTPACKET_RESULT_NO_ROOM_IN_BUFFER:
			{
				/* still connected */
				queueLock.Release();
				return;
			}break;

		default:
			{
				/* kill everything in the buffer */
				while((pck == _queue.Pop()))
					delete pck;
				queueLock.Release();
				return;
			}break;
		}
	}
	queueLock.Release();
}

OUTPACKET_RESULT WorldSocket::_OutPacket(uint16 opcode, size_t len, const void* data)
{
	bool rv;
	if(!IsConnected())
		return OUTPACKET_RESULT_NOT_CONNECTED;

	BurstBegin();
	//if((m_writeByteCount + len + 4) >= m_writeBufferSize)
	if( GetWriteBuffer().GetSpace() < (len+4) )
	{
		BurstEnd();
		return OUTPACKET_RESULT_NO_ROOM_IN_BUFFER;
	}

	// Packet logger :)
	sWorldLog.LogPacket((uint32)len, opcode, (const uint8*)data, 1);

	// Encrypt the packet
	// First, create the header.
	ServerPktHeader Header;
#ifdef USING_BIG_ENDIAN
	Header.size = len + 2;
	Header.cmd = swap16(opcode);
#else
	Header.cmd = opcode;
	Header.size = ntohs((uint16)len + 2);
#endif
    _crypt.EncryptFourSend((uint8*)&Header);

	// Pass the header to our send buffer
	rv = BurstSend((const uint8*)&Header, 4);

	// Pass the rest of the packet to our send buffer (if there is any)
	if(len > 0 && rv)
	{
		rv = BurstSend((const uint8*)data, (uint32)len);
	}

	if(rv) BurstPush();
	BurstEnd();
	return rv ? OUTPACKET_RESULT_SUCCESS : OUTPACKET_RESULT_SOCKET_ERROR;
}

void WorldSocket::OnConnect()
{
	sWorld.mAcceptedConnections++;
	_latency = getMSTime();

#ifdef USING_BIG_ENDIAN
	uint32 swapped = swap32(mSeed);
	OutPacket(SMSG_AUTH_CHALLENGE, 4, &swapped);
#else
	OutPacket(SMSG_AUTH_CHALLENGE, 4, &mSeed);
#endif
}

void WorldSocket::_HandleAuthSession(WorldPacket* recvPacket)
{
	std::string account;
	uint32 unk2;
	_latency = getMSTime() - _latency;

	try
	{
		*recvPacket >> mClientBuild;
		*recvPacket >> unk2;
		*recvPacket >> account;
		*recvPacket >> mClientSeed;
	}
	catch(ByteBuffer::error &)
	{
		sLog.outDetail("Incomplete copy of AUTH_SESSION Received.");
		return;
	}

	// Send out a request for this account.
	mRequestID = sLogonCommHandler.ClientConnected(account, this);
	
	if(mRequestID == 0xFFFFFFFF)
	{
		Disconnect();
		return;
	}

	// shitty hash !
	m_fullAccountName = new string( account );

	// Set the authentication packet 
    pAuthenticationPacket = recvPacket;
}

void WorldSocket::InformationRetreiveCallback(WorldPacket & recvData, uint32 requestid)
{
	if(requestid != mRequestID)
		return;

	uint32 error;
	recvData >> error;

	if(error != 0 || pAuthenticationPacket == NULL)
	{
		// something happened wrong @ the logon server
		OutPacket(SMSG_AUTH_RESPONSE, 1, "\x0D");
		return;
	}

	// Extract account information from the packet.
	string AccountName;
	const string * ForcedPermissions;
	uint32 AccountID;
	string GMFlags;
	uint8 AccountFlags;
	string lang = "enUS";
	uint32 i;
	
	recvData >> AccountID >> AccountName >> GMFlags >> AccountFlags;
	ForcedPermissions = sLogonCommHandler.GetForcedPermissions(AccountName);
	if( ForcedPermissions != NULL )
		GMFlags.assign(ForcedPermissions->c_str());

	sLog.outDebug( " >> got information packet from logon: `%s` ID %u (request %u)", AccountName.c_str(), AccountID, mRequestID);
//	sLog.outColor(TNORMAL, "\n");

	mRequestID = 0;
	// Pull the session key.
	uint8 K[40];
	recvData.read(K, 40);
	
	BigNumber BNK;
	BNK.SetBinary(K, 40);

	uint8 *key = new uint8[20];
	PacketKeyGenerator PKG;
	memcpy(key, PKG.GenerateKey(K), 20);
	
	// Initialize crypto.
	_crypt.SetKey(key, 20);
	_crypt.Init();
	delete key;

//	// Initialize crypto.
//	_crypt.SetKey(K, 40);
//	_crypt.Init();

	//checking if player is already connected
    //disconnect corrent player and login this one(blizzlike)

	if(recvData.rpos() != recvData.wpos())
		recvData.read((uint8*)lang.data(), 4);

	WorldSession *session = sWorld.FindSession( AccountID );
	if( session)
	{
		// AUTH_FAILED = 0x0D
		session->Disconnect();
		
		// clear the logout timer so he times out straight away
		session->SetLogoutTimer(1);

		// we must send authentication failed here.
		// the stupid newb can relog his client.
		// otherwise accounts dupe up and disasters happen.
		OutPacket(SMSG_AUTH_RESPONSE, 1, "\x15");
		return;
	}

	Sha1Hash sha;

	uint8 digest[20];
	pAuthenticationPacket->read(digest, 20);

	uint32 t = 0;
	if( m_fullAccountName == NULL )				// should never happen !
		sha.UpdateData(AccountName);
	else
	{
		sha.UpdateData(*m_fullAccountName);
		
		// this is unused now. we may as well free up the memory.
		delete m_fullAccountName;
		m_fullAccountName = NULL;
	}

	sha.UpdateData((uint8 *)&t, 4);
	sha.UpdateData((uint8 *)&mClientSeed, 4);
	sha.UpdateData((uint8 *)&mSeed, 4);
	sha.UpdateBigNumbers(&BNK, NULL);
	sha.Finalize();

	if (memcmp(sha.GetDigest(), digest, 20))
	{
		// AUTH_UNKNOWN_ACCOUNT = 21
		OutPacket(SMSG_AUTH_RESPONSE, 1, "\x15");
		return;
	}

	// Allocate session
	WorldSession * pSession = new WorldSession(AccountID, AccountName, this);
	mSession = pSession;
	ASSERT(mSession);
	pSession->deleteMutex.Acquire();
	
	// Set session properties
	pSession->SetClientBuild(mClientBuild);
	pSession->LoadSecurity(GMFlags);
	pSession->SetAccountFlags(AccountFlags);
	pSession->m_lastPing = (uint32)UNIXTIME;
	pSession->language = sLocalizationMgr.GetLanguageId(lang);

	if(recvData.rpos() != recvData.wpos())
		recvData >> pSession->m_muted;

	for(uint32 i = 0; i < 8; ++i)
		pSession->SetAccountData(i, NULL, true, 0);

	// queue the account loading
	/*AsyncQuery * aq = new AsyncQuery( new SQLClassCallbackP1<World, uint32>(World::getSingletonPtr(), &World::LoadAccountDataProc, AccountID) );
	aq->AddQuery("SELECT * FROM account_data WHERE acct = %u", AccountID);
	CharacterDatabase.QueueAsyncQuery(aq);*/
	if(sWorld.m_useAccountData)
	{
		QueryResult * pResult = CharacterDatabase.Query("SELECT * FROM account_data WHERE acct = %u", AccountID);
		if( pResult == NULL )
			CharacterDatabase.Execute("INSERT INTO account_data VALUES(%u, '', '', '', '', '', '', '', '', '')", AccountID);
		else
		{
			size_t len;
			const char * data;
			char * d;
			for(i = 0; i < 8; ++i)
			{
				data = pResult->Fetch()[1+i].GetString();
				len = data ? strlen(data) : 0;
				if(len > 1)
				{
					d = new char[len+1];
					memcpy(d, data, len+1);
					pSession->SetAccountData(i, d, true, (uint32)len);
				}
			}

			delete pResult;
		}
	}

	Log.Debug("Auth", "%s from %s:%u [%ums]", AccountName.c_str(), GetRemoteIP().c_str(), GetRemotePort(), _latency);
#ifdef SESSION_CAP
	if( sWorld.GetSessionCount() >= SESSION_CAP )
	{
		OutPacket(SMSG_AUTH_RESPONSE, 1, "\x0D");
		Disconnect();
		return;
	}
#endif

	// Check for queue.
	if( (sWorld.GetSessionCount() < sWorld.GetPlayerLimit()) || pSession->HasGMPermissions() ) {
		Authenticate();
	} else {
		// Queued, sucker.
		uint32 Position = sWorld.AddQueuedSocket(this);
		mQueued = true;
		Log.Debug("Queue", "%s added to queue in position %u", AccountName.c_str(), Position);

		// Send packet so we know what we're doing
		UpdateQueuePosition(Position);
	}

	pSession->deleteMutex.Release();
}

void WorldSocket::Authenticate()
{
	WorldSession * pSession = mSession;
	ASSERT(pAuthenticationPacket);
	mQueued = false;

	if(!pSession) return;
	pSession->deleteMutex.Acquire();

	if(pSession->HasFlag(ACCOUNT_FLAG_XPACK_01))
		OutPacket(SMSG_AUTH_RESPONSE, 11, "\x0C\x30\x78\x00\x00\x00\x00\x00\x00\x00\x01");
	else
		OutPacket(SMSG_AUTH_RESPONSE, 11, "\x0C\x30\x78\x00\x00\x00\x00\x00\x00\x00\x00");

	sAddonMgr.SendAddonInfoPacket(pAuthenticationPacket, (uint32)pAuthenticationPacket->rpos(), pSession);
	pSession->_latency = _latency;

	delete pAuthenticationPacket;
	pAuthenticationPacket = 0;

	if(mSession)
	{
		sWorld.AddSession(mSession);
		sWorld.AddGlobalSession(mSession);

/*		if(pSession->HasFlag(ACCOUNT_FLAG_XTEND_INFO))
			sWorld.AddExtendedSession(pSession);*/

		if(pSession->HasGMPermissions() && mSession)
			sWorld.gmList.insert(pSession);
	}

	pSession->deleteMutex.Release();
}

void WorldSocket::UpdateQueuePosition(uint32 Position)
{
	WorldPacket QueuePacket(SMSG_AUTH_RESPONSE, 15);
	QueuePacket << uint8(0x1B) << uint8(0x2C) << uint8(0x73) << uint8(0) << uint8(0);
	QueuePacket << uint32(0) << uint8(0);
	QueuePacket << Position;
	SendPacket(&QueuePacket);
}

void WorldSocket::_HandlePing(WorldPacket* recvPacket)
{
	uint32 ping;
	if(recvPacket->size() < 4)
	{
		sLog.outString("Socket closed due to incomplete ping packet.");
		Disconnect();
		return;
	}

	*recvPacket >> ping;
	*recvPacket >> _latency;

	if(mSession)
	{
		mSession->_latency = _latency;
		mSession->m_lastPing = (uint32)UNIXTIME;

		// reset the move time diff calculator, don't worry it will be re-calculated next movement packet.
		mSession->m_clientTimeDelay = 0;
	}

#ifdef USING_BIG_ENDIAN
	swap32(&ping);
#endif

	OutPacket(SMSG_PONG, 4, &ping);

#ifdef WIN32
	// Dynamically change nagle buffering status based on latency.
	if(_latency >= 250)
	{
		if(!m_nagleEanbled)
		{
			u_long arg = 0;
			setsockopt(GetFd(), 0x6, 0x1, (const char*)&arg, sizeof(arg));
			m_nagleEanbled = true;
		}
	}
	else
	{
		if(m_nagleEanbled)
		{
			u_long arg = 1;
			setsockopt(GetFd(), 0x6, 0x1, (const char*)&arg, sizeof(arg));
			m_nagleEanbled = false;
		}
	}
#endif
}

void WorldSocket::OnRead()
{
	for(;;)
	{
		// Check for the header if we don't have any bytes to wait for.
		if(mRemaining == 0)
		{
			if(GetReadBuffer().GetSize() < 6)
			{
				// No header in the packet, let's wait.
				return;
			}

			// Copy from packet buffer into header local var
			ClientPktHeader Header;
			GetReadBuffer().Read((uint8*)&Header, 6);

			// Decrypt the header
            _crypt.DecryptSixRecv((uint8*)&Header);
#ifdef USING_BIG_ENDIAN
			mRemaining = mSize = Header.size - 4;
			mOpcode = swap32(Header.cmd);
#else
			mRemaining = mSize = ntohs(Header.size) - 4;
			mOpcode = Header.cmd;
#endif
		}

		WorldPacket * Packet;

		if(mRemaining > 0)
		{
			if( GetReadBuffer().GetSize() < mRemaining )
			{
				// We have a fragmented packet. Wait for the complete one before proceeding.
				return;
			}
		}

		Packet = new WorldPacket(mOpcode, mSize);
		Packet->resize(mSize);

		if(mRemaining > 0)
		{
			// Copy from packet buffer into our actual buffer.
			///Read(mRemaining, (uint8*)Packet->contents());
			GetReadBuffer().Read((uint8*)Packet->contents(), mRemaining);
		}

		sWorldLog.LogPacket(mSize, mOpcode, mSize ? Packet->contents() : NULL, 0);
		mRemaining = mSize = mOpcode = 0;

		// Check for packets that we handle
		switch(Packet->GetOpcode())
		{
		case CMSG_PING:
			{
				_HandlePing(Packet);
				delete Packet;
			}break;
		case CMSG_AUTH_SESSION:
			{
				_HandleAuthSession(Packet);
			}break;
		default:
			{
				if(mSession) mSession->QueuePacket(Packet);
				else delete Packet;
			}break;
		}
	}
}

#endif

void WorldLog::LogPacket(uint32 len, uint16 opcode, const uint8* data, uint8 direction)
{
#ifdef ECHO_PACKET_LOG_TO_CONSOLE
	sLog.outString("[%s]: %s %s (0x%03X) of %u bytes.", direction ? "SERVER" : "CLIENT", direction ? "sent" : "received",
		LookupName(opcode, g_worldOpcodeNames), opcode, len);
#endif

	if(bEnabled)
	{
		mutex.Acquire();
		unsigned int line = 1;
		unsigned int countpos = 0;
		uint16 lenght = len;
		unsigned int count = 0;

		fprintf(m_file, "{%s} Packet: (0x%04X) %s PacketSize = %u\n", (direction ? "SERVER" : "CLIENT"), opcode,
			LookupName(opcode, g_worldOpcodeNames), lenght);
		fprintf(m_file, "|------------------------------------------------|----------------|\n");
		fprintf(m_file, "|00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F |0123456789ABCDEF|\n");
		fprintf(m_file, "|------------------------------------------------|----------------|\n");

		if(lenght > 0)
		{
			fprintf(m_file, "|");
			for (count = 0 ; count < lenght ; count++)
			{
				if (countpos == 16)
				{
					countpos = 0;

					fprintf(m_file, "|");

					for (unsigned int a = count-16; a < count;a++)
					{
						if ((data[a] < 32) || (data[a] > 126))
							fprintf(m_file, ".");
						else
							fprintf(m_file, "%c",data[a]);
					}

					fprintf(m_file, "|\n");

					line++;
					fprintf(m_file, "|");
				}

				fprintf(m_file, "%02X ",data[count]);

				//FIX TO PARSE PACKETS WITH LENGHT < OR = TO 16 BYTES.
				if (count+1 == lenght && lenght <= 16)
				{
					for (unsigned int b = countpos+1; b < 16;b++)
						fprintf(m_file, "   ");

					fprintf(m_file, "|");

					for (unsigned int a = 0; a < lenght;a++)
					{
						if ((data[a] < 32) || (data[a] > 126))
							fprintf(m_file, ".");
						else
							fprintf(m_file, "%c",data[a]);
					}

					for (unsigned int c = count; c < 15;c++)
						fprintf(m_file, " ");

					fprintf(m_file, "|\n");
				}

				//FIX TO PARSE THE LAST LINE OF THE PACKETS WHEN THE LENGHT IS > 16 AND ITS IN THE LAST LINE.
				if (count+1 == lenght && lenght > 16)
				{
					for (unsigned int b = countpos+1; b < 16;b++)
						fprintf(m_file, "   ");

					fprintf(m_file, "|");

					unsigned short print = 0;

					for (unsigned int a = line * 16 - 16; a < lenght;a++)
					{
						if ((data[a] < 32) || (data[a] > 126))
							fprintf(m_file, ".");
						else
							fprintf(m_file, "%c",data[a]);

						print++;
					}

					for (unsigned int c = print; c < 16;c++)
						fprintf(m_file, " ");

					fprintf(m_file, "|\n");
				}

				countpos++;
			}
		}
		fprintf(m_file, "-------------------------------------------------------------------\n\n");
		fflush(m_file);
		mutex.Release();
	}
}
