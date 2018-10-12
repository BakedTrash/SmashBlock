/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <base/system.h>

#include <engine/console.h>
#include <engine/external/md5/md5.h>
#include <engine/shared/config.h>

#include "netban.h"
#include "network.h"


bool CNetServer::Open(NETADDR BindAddr, CNetBan *pNetBan, int MaxClients, int MaxClientsPerIP, int Flags)
{
	// zero out the whole structure
	mem_zero(this, sizeof(*this));

	// open socket
	m_Socket = net_udp_create(BindAddr, 0);
	if(!m_Socket.type)
		return false;

	m_pNetBan = pNetBan;

	// clamp clients
	m_MaxClients = MaxClients;
	if(m_MaxClients > NET_MAX_CLIENTS)
		m_MaxClients = NET_MAX_CLIENTS;
	if(m_MaxClients < 1)
		m_MaxClients = 1;

	m_MaxClientsPerIP = MaxClientsPerIP;

	secure_random_fill(m_aaSalts, sizeof(m_aaSalts));
	m_LastSaltUpdate = time_get();

	for(int i = 0; i < NET_MAX_CLIENTS; i++)
		m_aSlots[i].m_Connection.Init(m_Socket, !g_Config.m_Debug);

	return true;
}

int CNetServer::SetCallbacks(NETFUNC_NEWCLIENT pfnNewClient, NETFUNC_DELCLIENT pfnDelClient, void *pUser)
{
	m_pfnNewClient = pfnNewClient;
	m_pfnDelClient = pfnDelClient;
	m_UserPtr = pUser;
	return 0;
}

int CNetServer::Close()
{
	// TODO: implement me
	return 0;
}

int CNetServer::Drop(int ClientID, const char *pReason)
{
	// TODO: insert lots of checks here
	/*NETADDR Addr = ClientAddr(ClientID);

	dbg_msg("net_server", "client dropped. cid=%d ip=%d.%d.%d.%d reason=\"%s\"",
		ClientID,
		Addr.ip[0], Addr.ip[1], Addr.ip[2], Addr.ip[3],
		pReason
		);*/
	if(m_pfnDelClient)
		m_pfnDelClient(ClientID, pReason, m_UserPtr);

	m_aSlots[ClientID].m_Connection.Disconnect(pReason);

	return 0;
}

int CNetServer::Update()
{
	int64 Now = time_get();
	int64 Freq = time_freq();
	if(Now >= m_LastSaltUpdate + 10 * Freq)
	{
		m_CurrentSalt = (m_CurrentSalt + 1) % (sizeof(m_aaSalts) / sizeof(m_aaSalts[0]));
		secure_random_fill(m_aaSalts[m_CurrentSalt], sizeof(m_aaSalts[m_CurrentSalt]));
		m_LastSaltUpdate = Now;
	}
	for(int i = 0; i < MaxClients(); i++)
	{
		m_aSlots[i].m_Connection.Update();
		if(m_aSlots[i].m_Connection.State() == NET_CONNSTATE_ERROR)
		{
			if(Now - m_aSlots[i].m_Connection.ConnectTime() < Freq && NetBan())
			{
				if(NetBan()->BanAddr(ClientAddr(i), 60, "Stressing network") == -1)
					Drop(i, m_aSlots[i].m_Connection.ErrorString());
			}
			else
				Drop(i, m_aSlots[i].m_Connection.ErrorString());
		}
	}

	return 0;
}

unsigned CNetServer::GetToken(const NETADDR &Addr) const
{
	return GetToken(Addr, m_CurrentSalt);
}

unsigned CNetServer::GetToken(const NETADDR &Addr, int SaltIndex) const
{
	unsigned char aDigest[16];
	md5_state_t Md5;
	md5_init(&Md5);
	md5_append(&Md5, (unsigned char *)&Addr, sizeof(Addr));
	md5_append(&Md5, m_aaSalts[SaltIndex], sizeof(m_aaSalts[SaltIndex]));
	md5_finish(&Md5, aDigest);
	return uint32_from_be(aDigest);
}

bool CNetServer::IsCorrectToken(const NETADDR &Addr, unsigned Token) const
{
	for(unsigned i = 0; i < sizeof(m_aaSalts) / sizeof(m_aaSalts[0]); i++)
	{
		if(GetToken(Addr, i) == Token)
		{
			return true;
		}
	}
	return false;
}

/*
	TODO: chopp up this function into smaller working parts
*/
int CNetServer::Recv(CNetChunk *pChunk)
{
	while(1)
	{
		NETADDR Addr;

		// check for a chunk
		if(m_RecvUnpacker.FetchChunk(pChunk))
			return 1;

		// TODO: empty the recvinfo
		int Bytes = net_udp_recv(m_Socket, &Addr, m_RecvUnpacker.m_aBuffer, NET_MAX_PACKETSIZE);

		// no more packets for now
		if(Bytes <= 0)
			break;

		if(CNetBase::UnpackPacket(m_RecvUnpacker.m_aBuffer, Bytes, &m_RecvUnpacker.m_Data) == 0)
		{
			bool UseToken = false;
			unsigned Token = 0;
			if(m_RecvUnpacker.m_Data.m_Flags&NET_PACKETFLAG_TOKEN)
			{
				UseToken = true;
				Token = m_RecvUnpacker.m_Data.m_Token;
			}
			else if(m_RecvUnpacker.m_Data.m_Flags&NET_PACKETFLAG_CONTROL && m_RecvUnpacker.m_Data.m_aChunkData[0] == NET_CTRLMSG_CONNECT && m_RecvUnpacker.m_Data.m_DataSize >= 1+512)
			{
				UseToken = true;
				Token = uint32_from_be(&m_RecvUnpacker.m_Data.m_aChunkData[5]);
			}
			// check if we just should drop the packet
			char aBuf[128];
			if(NetBan() && NetBan()->IsBanned(&Addr, aBuf, sizeof(aBuf)))
			{
				// banned, reply with a message
				CNetBase::SendControlMsg(m_Socket, &Addr, 0, UseToken, Token, NET_CTRLMSG_CLOSE, aBuf, str_length(aBuf)+1);
				continue;
			}

			if(m_RecvUnpacker.m_Data.m_Flags&NET_PACKETFLAG_CONNLESS)
			{
				pChunk->m_Flags = NETSENDFLAG_CONNLESS;
				pChunk->m_ClientID = -1;
				pChunk->m_Address = Addr;
				pChunk->m_DataSize = m_RecvUnpacker.m_Data.m_DataSize;
				pChunk->m_pData = m_RecvUnpacker.m_Data.m_aChunkData;
				return 1;
			}
			else
			{
				int ClientID = -1;

				// check if we already got this client
				for(int i = 0; i < MaxClients(); i++)
				{
					if(m_aSlots[i].m_Connection.State() != NET_CONNSTATE_OFFLINE &&
						net_addr_comp(m_aSlots[i].m_Connection.PeerAddress(), &Addr) == 0)
					{
						ClientID = i;
						break;
					}
				}

				if(m_RecvUnpacker.m_Data.m_Flags&NET_PACKETFLAG_CONTROL && m_RecvUnpacker.m_Data.m_aChunkData[0] == NET_CTRLMSG_CONNECT)
				{
					if(ClientID != -1)
					{
						continue; // silent ignore.. we got this client already
					}
					if(m_RecvUnpacker.m_Data.m_DataSize < 1+512)
					{
						if(g_Config.m_Debug)
						{
							dbg_msg("netserver", "dropping short connect packet, size=%d", m_RecvUnpacker.m_Data.m_DataSize);
						}
						continue;
					}

					unsigned MyToken = GetToken(Addr);
					unsigned char aConnectAccept[4];
					uint32_to_be(&aConnectAccept[0], MyToken);
					CNetBase::SendControlMsg(m_Socket, &Addr, 0, true, Token, NET_CTRLMSG_CONNECTACCEPT, aConnectAccept, sizeof(aConnectAccept));
				}
				else
				{
					// client that wants to connect
					if(ClientID == -1)
					{
						if(!UseToken || !IsCorrectToken(Addr, Token))
						{
							if(g_Config.m_Debug)
							{
									dbg_msg("netserver", "dropping packet with missing/invalid token, present=%d token=%08x", (int)UseToken, Token);
							}
							continue;
						}
						// only allow a specific number of players with the same ip
						NETADDR ThisAddr = Addr, OtherAddr;
						int FoundAddr = 1;
						ThisAddr.port = 0;
						for(int i = 0; i < MaxClients(); ++i)
						{
							if(m_aSlots[i].m_Connection.State() == NET_CONNSTATE_OFFLINE)
								continue;

							OtherAddr = *m_aSlots[i].m_Connection.PeerAddress();
							OtherAddr.port = 0;
							if(!net_addr_comp(&ThisAddr, &OtherAddr))
							{
								if(FoundAddr++ >= m_MaxClientsPerIP)
								{
									char aBuf[128];
									str_format(aBuf, sizeof(aBuf), "Only %d players with the same IP are allowed", m_MaxClientsPerIP);
									CNetBase::SendControlMsg(m_Socket, &Addr, 0, UseToken, Token, NET_CTRLMSG_CLOSE, aBuf, str_length(aBuf) + 1);
									return 0;
								}
							}
						}

						int EmptySlot = -1;
						for(int i = 0; i < MaxClients(); i++)
						{
							if(m_aSlots[i].m_Connection.State() == NET_CONNSTATE_OFFLINE)
							{
								EmptySlot = i;
								break;
							}
						}

						if(EmptySlot == -1)
						{
							const char aFullMsg[] = "This server is full";
							CNetBase::SendControlMsg(m_Socket, &Addr, 0, UseToken, Token, NET_CTRLMSG_CLOSE, aFullMsg, sizeof(aFullMsg));
						}
						else
						{
							m_aSlots[EmptySlot].m_Connection.Accept(&Addr, Token);
							m_aSlots[EmptySlot].m_Connection.Feed(&m_RecvUnpacker.m_Data, &Addr);
							if(m_pfnNewClient)
								m_pfnNewClient(EmptySlot, m_UserPtr);
						}
					}
					else
					{
						// normal packet
						if(m_RecvUnpacker.m_Data.m_DataSize)
							m_RecvUnpacker.Start(&Addr, &m_aSlots[ClientID].m_Connection, ClientID);
					}
				}
			}
		}
	}
	return 0;
}

int CNetServer::Send(CNetChunk *pChunk)
{
	if(pChunk->m_DataSize >= NET_MAX_PAYLOAD)
	{
		dbg_msg("netserver", "packet payload too big. %d. dropping packet", pChunk->m_DataSize);
		return -1;
	}

	if(pChunk->m_Flags&NETSENDFLAG_CONNLESS)
	{
		// send connectionless packet
		CNetBase::SendPacketConnless(m_Socket, &pChunk->m_Address, pChunk->m_pData, pChunk->m_DataSize);
	}
	else
	{
		int Flags = 0;
		dbg_assert(pChunk->m_ClientID >= 0, "errornous client id");
		dbg_assert(pChunk->m_ClientID < MaxClients(), "errornous client id");

		if(pChunk->m_Flags&NETSENDFLAG_VITAL)
			Flags = NET_CHUNKFLAG_VITAL;

		if(m_aSlots[pChunk->m_ClientID].m_Connection.QueueChunk(Flags, pChunk->m_DataSize, pChunk->m_pData) == 0)
		{
			if(pChunk->m_Flags&NETSENDFLAG_FLUSH)
				m_aSlots[pChunk->m_ClientID].m_Connection.Flush();
		}
		else
		{
			Drop(pChunk->m_ClientID, "Error sending data");
		}
	}
	return 0;
}

void CNetServer::SetMaxClientsPerIP(int Max)
{
	// clamp
	if(Max < 1)
		Max = 1;
	else if(Max > NET_MAX_CLIENTS)
		Max = NET_MAX_CLIENTS;

	m_MaxClientsPerIP = Max;
}
