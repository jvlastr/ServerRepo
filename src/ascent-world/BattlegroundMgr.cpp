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

#include "StdAfx.h"

#define ENABLE_AB
#define ENABLE_WSG
//#define ENABLE_AV
#define ENABLE_EOTS
#define ALLOWED_DISTANCE_AT_START 1600 // 40 yards
extern DayWatcherThread * dw;

initialiseSingleton(CBattlegroundManager);
typedef CBattleground*(*CreateBattlegroundFunc)(MapMgr* mgr,uint32 iid,uint32 group, uint32 type);

const static uint32 BGMapIds[BATTLEGROUND_NUM_TYPES] = {
	0,		// 0
	30,		// AV
	489,	// WSG
	529,	// AB
	0,		// 2v2
	0,		// 3v3
	0,		// 5v5
	566,	// Netherstorm BG
};

const static CreateBattlegroundFunc BGCFuncs[BATTLEGROUND_NUM_TYPES] = {
	NULL,						// 0
#ifdef ENABLE_AV
	&AlteracValley::Create,		// AV
#else
	NULL,						//AV
#endif
#ifdef ENABLE_WSG
	&WarsongGulch::Create,		// WSG
#else
	NULL,						// WSG
#endif
#ifdef ENABLE_AB
	&ArathiBasin::Create,		// AB
#else
	NULL,						// AB
#endif
	NULL,						// 2v2
	NULL,						// 3v3
	NULL,						// 5v5
#ifdef ENABLE_EOTS
	&EyeOfTheStorm::Create,		// Netherstorm
#else
	NULL,						// Netherstorm
#endif
};

const static uint32 BGMinimumPlayers[BATTLEGROUND_NUM_TYPES] = {
	0,							// 0
	20,							// AV
	5,							// WSG
	5,							// AB
	4,							// 2v2
	6,							// 3v3
	10,							// 5v5
	5,							// Netherstorm
};

CBattlegroundManager::CBattlegroundManager() : EventableObject()
{
	m_maxBattlegroundId = 0;
	memset(m_queuedPlayersCount, 0, BATTLEGROUND_NUM_TYPES*MAX_LEVEL_GROUP*2*sizeof(uint32));
	sEventMgr.AddEvent(this, &CBattlegroundManager::EventQueueUpdate, false, EVENT_BATTLEGROUND_QUEUE_UPDATE, 15000, 0,0);
}

CBattlegroundManager::~CBattlegroundManager()
{

}

void CBattlegroundManager::HandleBattlegroundListPacket(WorldSession * m_session, uint32 BattlegroundType)
{
	if(BattlegroundType == BATTLEGROUND_ARENA_2V2 || BattlegroundType == BATTLEGROUND_ARENA_3V3 || BattlegroundType == BATTLEGROUND_ARENA_5V5)
	{
		WorldPacket data(SMSG_BATTLEFIELD_LIST, 17);
		data << m_session->GetPlayer()->GetGUID() << uint32(6) << uint32(0xC) << uint8(0);
		m_session->SendPacket(&data);
		return;
	}

	uint32 LevelGroup = GetLevelGrouping(m_session->GetPlayer()->getLevel());
	uint32 Count = 0;
	WorldPacket data(SMSG_BATTLEFIELD_LIST, 200);
	data << m_session->GetPlayer()->GetGUID();
	data << BattlegroundType;
	data << uint8(2);
	data << uint32(0);		// Count

	/* Append the battlegrounds */
	m_instanceLock.Acquire();
	for(map<uint32, CBattleground*>::iterator itr = m_instances[BattlegroundType].begin(); itr != m_instances[BattlegroundType].end(); ++itr)
	{
        if( itr->second->GetLevelGroup() == LevelGroup && itr->second->CanPlayerJoin(m_session->GetPlayer()) && !itr->second->HasEnded() )
		{
			data << itr->first;
			++Count;
		}
	}
	m_instanceLock.Release();
#ifdef USING_BIG_ENDIAN
	*(uint32*)&data.contents()[13] = swap32(Count);
#else
	*(uint32*)&data.contents()[13] = Count;
#endif
	m_session->SendPacket(&data);
}

void CBattlegroundManager::HandleBattlegroundJoin(WorldSession * m_session, WorldPacket & pck)
{
	uint64 guid;
	uint32 pguid = m_session->GetPlayer()->GetLowGUID();
	uint32 lgroup = GetLevelGrouping(m_session->GetPlayer()->getLevel());
	uint32 bgtype;
	uint32 instance;

	pck >> guid >> bgtype >> instance;
	
	if(bgtype >= BATTLEGROUND_NUM_TYPES || !bgtype || !guid)
	{
		m_session->Disconnect();
		return;		// cheater!
	}

	/* Check the instance id */
	if(instance)
	{
		/* We haven't picked the first instance. This means we've specified an instance to join. */
		m_instanceLock.Acquire();
		map<uint32, CBattleground*>::iterator itr = m_instances[bgtype].find(instance);

		if(itr == m_instances[bgtype].end())
		{
			sChatHandler.SystemMessage(m_session, "You have tried to join an invalid instance id.");
			m_instanceLock.Release();
			return;
		}

		m_instanceLock.Release();
	}
    
	/* Queue him! */
	m_queueLock.Acquire();
	if(sWorld.BGQueueDisplay)
		sChatHandler.SystemMessage(m_session, "Players in this queue: Alliance %u, Horde %u", m_queuedPlayersCount[bgtype][lgroup][0], m_queuedPlayersCount[bgtype][lgroup][1]);
	m_queuedPlayers[bgtype][lgroup].push_back(pguid);
	Log.Success("BattlegroundManager", "Player %u is now in battleground queue for instance %u", m_session->GetPlayer()->GetLowGUID(), instance );

	/* send the battleground status packet */
	SendBattlefieldStatus(m_session->GetPlayer(), 1, bgtype, instance, 0, BGMapIds[bgtype],0);
	m_session->GetPlayer()->m_bgIsQueued = true;
	m_session->GetPlayer()->m_bgQueueInstanceId = instance;
	m_session->GetPlayer()->m_bgQueueType = bgtype;

	/* Set battleground entry point */
	m_session->GetPlayer()->m_bgEntryPointX = m_session->GetPlayer()->GetPositionX();
	m_session->GetPlayer()->m_bgEntryPointY = m_session->GetPlayer()->GetPositionY();
	m_session->GetPlayer()->m_bgEntryPointZ = m_session->GetPlayer()->GetPositionZ();
	m_session->GetPlayer()->m_bgEntryPointMap = m_session->GetPlayer()->GetMapId();
	m_session->GetPlayer()->m_bgEntryPointInstance = m_session->GetPlayer()->GetInstanceID();

	m_queueLock.Release();

	/* We will get updated next few seconds =) */
}

#define IS_ARENA(x) ( (x) >= BATTLEGROUND_ARENA_2V2 && (x) <= BATTLEGROUND_ARENA_5V5 )

void ErasePlayerFromList(uint32 guid, list<uint32>* l)
{
	for(list<uint32>::iterator itr = l->begin(); itr != l->end(); ++itr)
	{
		if((*itr) == guid)
		{
			l->erase(itr);
			return;
		}
	}
}

void CBattlegroundManager::EventQueueUpdate(bool forceStart)
{
	deque<Player*> tempPlayerVec[2];
	uint32 i,j,k;
	Player * plr;
	CBattleground * bg;
	list<uint32>::iterator it3, it4;
	//vector<Player*>::iterator it6;
	map<uint32, CBattleground*>::iterator iitr;
	Arena * arena;
	int32 team;
	m_queueLock.Acquire();
	m_instanceLock.Acquire();

	for(i = 0; i < BATTLEGROUND_NUM_TYPES; ++i)
	{
		for(j = 0; j < MAX_LEVEL_GROUP; ++j)
		{
			if(!m_queuedPlayers[i][j].size())
				continue;

			tempPlayerVec[0].clear();
			tempPlayerVec[1].clear();

			for(it3 = m_queuedPlayers[i][j].begin(); it3 != m_queuedPlayers[i][j].end();)
			{
				it4 = it3++;
                plr = objmgr.GetPlayer(*it4);
				
				if(!plr || GetLevelGrouping(plr->getLevel()) != j)
				{
                    m_queuedPlayers[i][j].erase(it4);
					continue;
				}

				// queued to a specific instance id?
				if(plr->m_bgQueueInstanceId != 0)
				{
					iitr = m_instances[i].find(plr->m_bgQueueInstanceId);
					if(iitr == m_instances[i].end())
					{
						// queue no longer valid
						plr->GetSession()->SystemMessage("Your queue on battleground instance id %u is no longer valid. Reason: Instance Deleted.", plr->m_bgQueueInstanceId);
						plr->m_bgIsQueued = false;
						plr->m_bgQueueType = 0;
						plr->m_bgQueueInstanceId = 0;
						m_queuedPlayers[i][j].erase(it4);
					}

					// can we join?
					bg = iitr->second;
					if(bg->CanPlayerJoin(plr))
					{
						bg->AddPlayer(plr, plr->GetTeam());
						m_queuedPlayers[i][j].erase(it4);
					}
				}
				else
				{
					if(IS_ARENA(i))
						tempPlayerVec[0].push_back(plr);
					else
						tempPlayerVec[plr->GetTeam()].push_back(plr);
				}
			}

			// try to join existing instances
			for(iitr = m_instances[i].begin(); iitr != m_instances[i].end(); ++iitr)
			{
				if(!iitr->second || iitr->second->HasEnded() || iitr->second->GetLevelGroup() != j)
					continue;

				if(IS_ARENA(i))
				{
                    arena = ((Arena*)iitr->second);
					if(arena->Rated())
						continue;

					team = arena->GetFreeTeam();
					while(team >= 0 && tempPlayerVec[0].size())
					{
						plr = *tempPlayerVec[0].begin();
						tempPlayerVec[0].pop_front();
						plr->m_bgTeam=team;
						arena->AddPlayer(plr, team);
						ErasePlayerFromList(plr->GetLowGUID(), &m_queuedPlayers[i][j]);
						team = arena->GetFreeTeam();
					}
				}
				else
				{
					bg = iitr->second;
					for(k = 0; k < 2; ++k)
					{
						while(tempPlayerVec[k].size() && bg->HasFreeSlots(k))
						{
							plr = *tempPlayerVec[k].begin();
							tempPlayerVec[k].pop_front();
							bg->AddPlayer(plr, plr->GetTeam());
							ErasePlayerFromList(plr->GetLowGUID(), &m_queuedPlayers[i][j]);
						}
					}
				}
			}

			if(IS_ARENA(i))
			{
				if((forceStart && tempPlayerVec[0].size() >= 1) ||
				    tempPlayerVec[0].size() >= BGMinimumPlayers[i])
				{
					if(CanCreateInstance(i,j))
					{
						arena = ((Arena*)CreateInstance(i, j));
						if(!arena)
							continue;
						team = arena->GetFreeTeam();
						while(!arena->IsFull() && tempPlayerVec[0].size() && team >= 0)
						{
							plr = *tempPlayerVec[0].begin();
							tempPlayerVec[0].pop_front();

							plr->m_bgTeam=team;
							arena->AddPlayer(plr, team);
							team = arena->GetFreeTeam();

							// remove from the main queue (painful!)
							ErasePlayerFromList(plr->GetLowGUID(), &m_queuedPlayers[i][j]);
						}
					}
				}
			}
			else
			{
				if((forceStart && 
				   (tempPlayerVec[0].size() >= 1 ||
					tempPlayerVec[1].size() >= 1)) ||
					(tempPlayerVec[0].size() >= BGMinimumPlayers[i] &&
					 tempPlayerVec[1].size() >= BGMinimumPlayers[i]))
				{
					if(CanCreateInstance(i,j))
					{
						bg = CreateInstance(i,j);
						//Hackfix against WPE users. We should check where the real problem is.
						if(!bg)
						{
							for(k = 0; k < 2; ++k)
							{
								while(tempPlayerVec[k].size())
								{
									plr = *tempPlayerVec[k].begin();
									tempPlayerVec[k].pop_front();
									ErasePlayerFromList(plr->GetLowGUID(), &m_queuedPlayers[i][j]);
								}
							}
						continue;
						}
						
						// push as many as possible in
						for(k = 0; k < 2; ++k)
						{
							while(tempPlayerVec[k].size() && bg->HasFreeSlots(k))
							{
								plr = *tempPlayerVec[k].begin();
								tempPlayerVec[k].pop_front();
								plr->m_bgTeam=k;
								bg->AddPlayer(plr, k);
								ErasePlayerFromList(plr->GetLowGUID(), &m_queuedPlayers[i][j]);
							}
						}
					}
				}
			}
			m_queuedPlayersCount[i][j][0] = tempPlayerVec[0].size();
			m_queuedPlayersCount[i][j][1] = tempPlayerVec[1].size();
		}
	}

	/* Handle paired arena team joining */
	Group * group1, *group2;
	uint32 n;
	list<uint32>::iterator itz;
	for(i = BATTLEGROUND_ARENA_2V2; i < BATTLEGROUND_ARENA_5V5+1; ++i)
	{
		for(;;)
		{
			if(m_queuedGroups[i].size() < 2)		/* got enough to have an arena battle ;P */
			{
                break;				
			}

			group1 = group2 = NULL;
			while(group1 == NULL)
			{
				n = RandomUInt((uint32)m_queuedGroups[i].size()) - 1;
				for(itz = m_queuedGroups[i].begin(); itz != m_queuedGroups[i].end() && n>0; ++itz)
					--n;

				if(itz == m_queuedGroups[i].end())
					itz=m_queuedGroups[i].begin();

				if(itz == m_queuedGroups[i].end())
				{
					Log.Error("BattlegroundMgr", "Internal error at %s:%u", __FILE__, __LINE__);
					m_queueLock.Release();
					m_instanceLock.Release();
					return;
				}

				group1 = objmgr.GetGroupById(*itz);
				m_queuedGroups[i].erase(itz);
				
				if(group1 == NULL || group1->GetLeader()->m_loggedInPlayer == NULL || group1->GetSubGroup(0) == NULL)
					group1 = NULL;
			}

			while(group2 == NULL)
			{
				n = RandomUInt((uint32)m_queuedGroups[i].size()) - 1;
				for(itz = m_queuedGroups[i].begin(); itz != m_queuedGroups[i].end() && n>0; ++itz)
					--n;

				if(itz == m_queuedGroups[i].end())
					itz=m_queuedGroups[i].begin();

				if(itz == m_queuedGroups[i].end())
				{
					Log.Error("BattlegroundMgr", "Internal error at %s:%u", __FILE__, __LINE__);
					m_queueLock.Release();
					m_instanceLock.Release();
					return;
				}
				group2 = objmgr.GetGroupById(*itz);
				m_queuedGroups[i].erase(itz);

				if(group2 == NULL || group2->GetLeader()->m_loggedInPlayer == NULL || group2->GetSubGroup(0) == NULL)
					group2 = NULL;
			}

			Arena * ar = ((Arena*)CreateInstance(i,LEVEL_GROUP_70));
			if(!ar)
				continue;

			GroupMembersSet::iterator itx;
			ar->rated_match=true;

			for(itx = group1->GetSubGroup(0)->GetGroupMembersBegin(); itx != group1->GetSubGroup(0)->GetGroupMembersEnd(); ++itx)
			{
				if((*itx)->m_loggedInPlayer)
				{
					if( ar->HasFreeSlots(0) )
					{
						ar->AddPlayer((*itx)->m_loggedInPlayer, 0);
						(*itx)->m_loggedInPlayer->SetTeam(0);
					}
				}
			}

			for(itx = group2->GetSubGroup(0)->GetGroupMembersBegin(); itx != group2->GetSubGroup(0)->GetGroupMembersEnd(); ++itx)
			{
				if((*itx)->m_loggedInPlayer)
				{
					if( ar->HasFreeSlots(1) )
					{
						ar->AddPlayer((*itx)->m_loggedInPlayer, 1);
						(*itx)->m_loggedInPlayer->SetTeam(1);
					}
				}
			}
		}
	}

	m_queueLock.Release();
	m_instanceLock.Release();
}

void CBattlegroundManager::RemovePlayerFromQueues(Player * plr)
{
	m_queueLock.Acquire();

	ASSERT(plr->m_bgQueueType < BATTLEGROUND_NUM_TYPES);
	uint32 lgroup = GetLevelGrouping(plr->getLevel());
	list<uint32>::iterator itr = m_queuedPlayers[plr->m_bgQueueType][lgroup].begin();
	
	while(itr != m_queuedPlayers[plr->m_bgQueueType][lgroup].end())
	{
		if((*itr) == plr->GetLowGUID())
		{
			Log.Debug("BattlegroundManager", "Removing player %u from queue instance %u type %u", plr->GetLowGUID(), plr->m_bgQueueInstanceId, plr->m_bgQueueType);
			m_queuedPlayers[plr->m_bgQueueType][lgroup].erase(itr);
			break;
		}

		++itr;
	}

	plr->m_bgIsQueued = false;
	plr->m_bgTeam=plr->GetTeam();
	plr->m_pendingBattleground=0;
	SendBattlefieldStatus(plr,0,0,0,0,0,0);
    m_queueLock.Release();
}

void CBattlegroundManager::RemoveGroupFromQueues(Group * grp)
{
	m_queueLock.Acquire();
	for(uint32 i = BATTLEGROUND_ARENA_2V2; i < BATTLEGROUND_ARENA_5V5+1; ++i)
	{
		for(list<uint32>::iterator itr = m_queuedGroups[i].begin(); itr != m_queuedGroups[i].end(); )
		{
			if((*itr) == grp->GetID())
				itr = m_queuedGroups[i].erase(itr);
			else
				++itr;
		}
	}

	for(GroupMembersSet::iterator itr = grp->GetSubGroup(0)->GetGroupMembersBegin(); itr != grp->GetSubGroup(0)->GetGroupMembersEnd(); ++itr)
		if((*itr)->m_loggedInPlayer)
			SendBattlefieldStatus((*itr)->m_loggedInPlayer, 0, 0, 0, 0, 0, 0);

	m_queueLock.Release();
}


bool CBattlegroundManager::CanCreateInstance(uint32 Type, uint32 LevelGroup)
{
	/*uint32 lc = 0;
	for(map<uint32, CBattleground*>::iterator itr = m_instances[Type].begin(); itr != m_instances[Type].end(); ++itr)
	{
		if(itr->second->GetLevelGroup() == LevelGroup)
		{
			lc++;
			if(lc >= MAXIMUM_BATTLEGROUNDS_PER_LEVEL_GROUP)
				return false;
		}
	}*/

	return true;
}

void CBattleground::SendWorldStates(Player * plr)
{
	if(!m_worldStates.size())
		return;

	uint32 bflag = 0;
	uint32 bflag2 = 0;

	switch(m_mapMgr->GetMapId())
	{
	case  489: bflag = 0x0CCD; bflag2 = 0x0CF9; break;
	case  529: bflag = 0x0D1E; break;
	case   30: bflag = 0x0A25; break;
	case  559: bflag = 3698; break;
	case 566: bflag = 0x0eec; bflag2 = 0; break;			// EOTS
	
	default:		/* arenas */
		bflag  = 0x0E76;
		bflag2 = 0;
		break;
	}

	WorldPacket data(SMSG_INIT_WORLD_STATES, 10 + (m_worldStates.size() * 8));
	data << m_mapMgr->GetMapId();
	data << bflag;
	data << bflag2;
	data << uint16(m_worldStates.size());

	for(map<uint32, uint32>::iterator itr = m_worldStates.begin(); itr != m_worldStates.end(); ++itr)
		data << itr->first << itr->second;
	plr->GetSession()->SendPacket(&data);
}

CBattleground::CBattleground(MapMgr * mgr, uint32 id, uint32 levelgroup, uint32 type) : m_mapMgr(mgr), m_id(id), m_type(type), m_levelGroup(levelgroup)
{
	m_nextPvPUpdateTime = 0;
	m_countdownStage = 0;
	m_ended = false;
	m_started = false;
	m_winningteam = 0;
	m_startTime = (uint32)UNIXTIME;
	m_lastResurrect = (uint32)UNIXTIME;
	sEventMgr.AddEvent(this, &CBattleground::EventResurrectPlayers, EVENT_BATTLEGROUND_QUEUE_UPDATE, 30000, 0,0);

	/* create raid groups */
	for(uint32 i = 0; i < 2; ++i)
	{
		m_groups[i] = new Group(true);
		m_groups[i]->m_disbandOnNoMembers = false;
		m_groups[i]->ExpandToRaid();
	}
	m_honorPerKill = HonorHandler::CalculateHonorPointsForKill(m_levelGroup * 10, m_levelGroup * 10);
}

CBattleground::~CBattleground()
{
	sEventMgr.RemoveEvents(this);
	for(uint32 i = 0; i < 2; ++i)
	{
		PlayerInfo *inf;
		for(uint32 j = 0; j < m_groups[i]->GetSubGroupCount(); ++j) {
			for(GroupMembersSet::iterator itr = m_groups[i]->GetSubGroup(j)->GetGroupMembersBegin(); itr != m_groups[i]->GetSubGroup(j)->GetGroupMembersEnd();) {
				inf = (*itr);
				++itr;
				m_groups[i]->RemovePlayer(inf);
			}
		}
		delete m_groups[i];
	}
}

void CBattleground::UpdatePvPData()
{
	if(isArena())
	{
		if(!m_ended)
		{
			return;
		}
	}

	if(UNIXTIME >= m_nextPvPUpdateTime)
	{
		m_mainLock.Acquire();
		WorldPacket data(10*(m_players[0].size()+m_players[1].size())+50);
		BuildPvPUpdateDataPacket(&data);
		DistributePacketToAll(&data);
		m_mainLock.Release();

		m_nextPvPUpdateTime = UNIXTIME + 2;
	}
}

void CBattleground::BuildPvPUpdateDataPacket(WorldPacket * data)
{
	data->Initialize(MSG_PVP_LOG_DATA);
	data->reserve(10*(m_players[0].size()+m_players[1].size())+50);

	BGScore * bs;
	if(isArena())
	{
		*data << uint8(1);
		if(!Rated())
		{
			*data << uint32(0x61272A5C);
			*data << uint32(0);
			*data << uint8(0);
			*data << uint32(0x61272A5C);
			*data << uint32(0);
			*data << uint8(0);
		}
		else
		{
			/* Grab some arena teams */
			ArenaTeam * teams[2] = {NULL,NULL};
			for(uint32 i = 0; i < 2; ++i)
			{
				for(set<Player*>::iterator itr = m_players[i].begin(); itr != m_players[i].end(); ++itr)
				{
					teams[i] = (*itr)->m_arenaTeams[ ((Arena*)this)->GetArenaTeamType() ];
					if(teams[i])
						break;
				}
			}

			if(teams[0])
			{
				*data << uint32(teams[0]->m_id);
				*data << uint32(0);
				*data << teams[0]->m_name;
			}
			else
			{
				*data << uint32(0x61272A5C);
				*data << uint32(0);
				*data << uint8(0);
			}
			
			if(teams[1])
			{
				*data << uint32(teams[1]->m_id);
				*data << uint32(0);
				*data << teams[1]->m_name;
			}
			else
			{
				*data << uint32(m_players[0].size() + m_players[1].size());
				*data << uint32(0);
				*data << uint8(0);
			}
		}

		if(m_ended)
		{
			*data << uint8(1);
			*data << uint8(m_winningteam);
		}
		else
			*data << uint8(0);		// If the game has ended - this will be 1

		*data << uint32(m_players[0].size() + m_players[1].size());
		for(uint32 i = 0; i < 2; ++i)
		{
			for(set<Player*>::iterator itr = m_players[i].begin(); itr != m_players[i].end(); ++itr)
			{
				*data << (*itr)->GetGUID();
				bs = &(*itr)->m_bgScore;
				*data << bs->KillingBlows;

				*data << uint8((*itr)->m_bgTeam);
				
				*data << bs->DamageDone;
				*data << bs->HealingDone;
				
				*data << uint32(1);			// count of values after this
				*data << uint32(bs->Misc1);	// rating change
			}
		}
	}
	else
	{
		*data << uint8(0);
		if(m_ended)
		{
			*data << uint8(1);
			*data << uint8(m_winningteam);
		}
		else
			*data << uint8(0);		// If the game has ended - this will be 1

		*data << uint32(m_players[0].size() + m_players[1].size());
		for(uint32 i = 0; i < 2; ++i)
		{
			for(set<Player*>::iterator itr = m_players[i].begin(); itr != m_players[i].end(); ++itr)
			{
				*data << (*itr)->GetGUID();
				bs = &(*itr)->m_bgScore;

				*data << bs->KillingBlows;
				*data << bs->HonorableKills;
				*data << bs->Deaths;
				*data << bs->BonusHonor;
				*data << bs->DamageDone;
				*data << bs->HealingDone;
				*data << uint32(0x2);
				*data << bs->Misc1;
				*data << bs->Misc2;
			}
		}
	}

}
void CBattleground::AddPlayer(Player * plr, uint32 team)
{
	m_mainLock.Acquire();

	/* This is called when the player is added, not when they port. So, they're essentially still queued, but not inside the bg yet */
	m_pendPlayers[team].insert(plr->GetLowGUID());

	/* Send a packet telling them that they can enter */
	BattlegroundManager.SendBattlefieldStatus(plr, 2, m_type, m_id, 120000, m_mapMgr->GetMapId(),Rated());		// You will be removed from the queue in 2 minutes.

	/* Add an event to remove them in 2 minutes time. */
	sEventMgr.AddEvent(plr, &Player::RemoveFromBattlegroundQueue, EVENT_BATTLEGROUND_QUEUE_UPDATE, 120000, 1,0);
	plr->m_pendingBattleground = this;

	m_mainLock.Release();
}

void CBattleground::RemovePendingPlayer(Player * plr)
{
	m_mainLock.Acquire();
	m_pendPlayers[plr->m_bgTeam].erase(plr->GetLowGUID());

	/* send a null bg update (so they don't join) */
	BattlegroundManager.SendBattlefieldStatus(plr, 0, 0, 0, 0, 0,0);
	plr->m_pendingBattleground =0;
	plr->m_bgTeam=plr->GetTeam();

	m_mainLock.Release();
}

void CBattleground::OnPlayerPushed(Player * plr)
{
	if( plr->GetGroup() && !Rated() )
		plr->GetGroup()->RemovePlayer(plr->m_playerInfo);

	plr->ProcessPendingUpdates();
	
	if( plr->GetGroup() == NULL && !Rated() && !plr->m_isGmInvisible &&
		plr->m_bgTeam >= 0 && plr->m_bgTeam <= 1)
		m_groups[plr->m_bgTeam]->AddMember( plr->m_playerInfo );
}

void CBattleground::PortPlayer(Player * plr, bool skip_teleport /* = false*/)
{
	m_mainLock.Acquire();
	if(m_ended)
	{
		sChatHandler.SystemMessage(plr->GetSession(), "You cannot join this battleground as it has already ended.");
		BattlegroundManager.SendBattlefieldStatus(plr, 0, 0, 0, 0, 0,0);
		plr->m_pendingBattleground = 0;
		m_mainLock.Release();
		return;
	}

	m_pendPlayers[plr->m_bgTeam].erase(plr->GetLowGUID());
	if(m_players[plr->m_bgTeam].find(plr) != m_players[plr->m_bgTeam].end())
	{
		m_mainLock.Release();
		return;
	}

	plr->FullHPMP();
	plr->SetTeam(plr->m_bgTeam);
	if(!plr->m_isGmInvisible) // Don't announce GM joining
	{
		WorldPacket data(SMSG_BATTLEGROUND_PLAYER_JOINED, 8);
		data << plr->GetGUID();
		DistributePacketToTeam(&data, plr->m_bgTeam);

		m_players[plr->m_bgTeam].insert(plr);
	}

	/* remove from any auto queue remove events */
	sEventMgr.RemoveEvents(plr, EVENT_BATTLEGROUND_QUEUE_UPDATE);

	if( !skip_teleport )
	{
		if( plr->IsInWorld() )
			plr->RemoveFromWorld();
	}

	plr->m_pendingBattleground = 0;
	plr->m_bg = this;
	
	if(!plr->IsPvPFlagged())
		plr->SetPvPFlag();

	/* Reset the score */
	memset(&plr->m_bgScore, 0, sizeof(BGScore));

	/* send him the world states */
	SendWorldStates(plr);
	
	/* update pvp data */
	UpdatePvPData();

	/* add the player to the group */
	if(plr->GetGroup() && !Rated())
	{
		// remove them from their group
		plr->GetGroup()->RemovePlayer( plr->m_playerInfo );
	}

	if(!m_countdownStage)
	{
		m_countdownStage = 1;
		sEventMgr.AddEvent(this, &CBattleground::EventCountdown, EVENT_BATTLEGROUND_COUNTDOWN, 30000, 0,0);
		sEventMgr.ModifyEventTimeLeft(this, EVENT_BATTLEGROUND_COUNTDOWN, 10000);
	}

	sEventMgr.RemoveEvents(this, EVENT_BATTLEGROUND_CLOSE);
	OnAddPlayer(plr);

	if(!skip_teleport)
	{
		/* This is where we actually teleport the player to the battleground. */	
		plr->SafeTeleport(m_mapMgr,GetStartingCoords(plr->m_bgTeam));
		BattlegroundManager.SendBattlefieldStatus(plr, 3, m_type, m_id, (uint32)UNIXTIME - m_startTime, m_mapMgr->GetMapId(),Rated());	// Elapsed time is the last argument
	}

	m_mainLock.Release();
}

CBattleground * CBattlegroundManager::CreateInstance(uint32 Type, uint32 LevelGroup)
{
	CreateBattlegroundFunc cfunc = BGCFuncs[Type];
	MapMgr * mgr = 0;
	CBattleground * bg;
	uint32 iid;
	uint32 week;
	
	if(IS_ARENA(Type))
	{
		/* arenas follow a different procedure. */
		static const uint32 arena_map_ids[3] = { 559, 562, 572 };
		uint32 mapid = arena_map_ids[RandomUInt(2)];
		uint32 players_per_side;
		mgr = sInstanceMgr.CreateBattlegroundInstance(mapid);
		if(mgr == NULL)
		{
			Log.Error("BattlegroundManager", "Arena CreateInstance() call failed for map %u, type %u, level group %u", mapid, Type, LevelGroup);
			return NULL;		// Shouldn't happen
		}

		switch(Type)
		{
		case BATTLEGROUND_ARENA_2V2:
			players_per_side = 2;
			break;

		case BATTLEGROUND_ARENA_3V3:
			players_per_side = 3;
			break;

		case BATTLEGROUND_ARENA_5V5:
			players_per_side = 5;
			break;
        default:
            players_per_side = 0;
            break;
		}

		iid = ++m_maxBattlegroundId;
        bg = new Arena(mgr, iid, LevelGroup, Type, players_per_side);
		if(!bg)
			return NULL;
		mgr->m_battleground = bg;
		Log.Success("BattlegroundManager", "Created arena battleground type %u for level group %u on map %u.", Type, LevelGroup, mapid);
		sEventMgr.AddEvent(bg, &CBattleground::EventCreate, EVENT_BATTLEGROUND_QUEUE_UPDATE, 1, 1,0);
		m_instanceLock.Acquire();
		m_instances[Type].insert( make_pair(iid, bg) );
		m_instanceLock.Release();
		return bg;
	}


	if(cfunc == NULL)
	{
		Log.Error("BattlegroundManager", "Could not find CreateBattlegroundFunc pointer for type %u level group %u", Type, LevelGroup);
		return NULL;
	}

	switch (Type)
	{
		case BATTLEGROUND_WARSONG_GULCH:
			week = 0;
			break;
		case BATTLEGROUND_ARATHI_BASIN:
			week = 1;
			break;
		case BATTLEGROUND_EYE_OF_THE_STORM:
			week = 2;
			break;
		case BATTLEGROUND_ALTERAC_VALLEY:
			week = 3;
			break;
	}

	/* Create Map Manager */
	mgr = sInstanceMgr.CreateBattlegroundInstance(BGMapIds[Type]);
	if(mgr == NULL)
	{
		Log.Error("BattlegroundManager", "CreateInstance() call failed for map %u, type %u, level group %u", BGMapIds[Type], Type, LevelGroup);
		return NULL;		// Shouldn't happen
	}

	/* Call the create function */
	iid = ++m_maxBattlegroundId;
	bg = cfunc(mgr, iid, LevelGroup, Type);
	bg->m_isholiday = (dw->m_isholiday && dw->week_number == week);
	mgr->m_battleground = bg;
	sEventMgr.AddEvent(bg, &CBattleground::EventCreate, EVENT_BATTLEGROUND_QUEUE_UPDATE, 1, 1,0);
	Log.Success("BattlegroundManager", "Created battleground type %u for level group %u.", Type, LevelGroup);

	m_instanceLock.Acquire();
	m_instances[Type].insert( make_pair(iid, bg) );
	m_instanceLock.Release();

	return bg;
}

void CBattlegroundManager::DeleteBattleground(CBattleground * bg)
{
	uint32 i = bg->GetType();
	uint32 j = bg->GetLevelGroup();
	Player * plr;

	m_instanceLock.Acquire();
	m_queueLock.Acquire();
	m_instances[i].erase(bg->GetId());
	
	/* erase any queued players */
	list<uint32>::iterator itr = m_queuedPlayers[i][j].begin();
	list<uint32>::iterator it2;
	for(; itr != m_queuedPlayers[i][j].end();)
	{
		it2 = itr++;
		plr = objmgr.GetPlayer(*it2);
		if(!plr)
		{
			m_queuedPlayers[i][j].erase(it2);
			continue;
		}

		if(plr && plr->m_bgQueueInstanceId == bg->GetId())
		{
			sChatHandler.SystemMessageToPlr(plr, "Your queue on battleground instance %u is no longer valid, the instance no longer exists.", bg->GetId());
			SendBattlefieldStatus(plr, 0, 0, 0, 0, 0,0);
			plr->m_bgIsQueued = false;
			m_queuedPlayers[i][j].erase(it2);
		}
	}

	m_queueLock.Release();
	m_instanceLock.Release();

}

GameObject * CBattleground::SpawnGameObject(uint32 entry,uint32 MapId , float x, float y, float z, float o, uint32 flags, uint32 faction, float scale)
{
	GameObject *go = m_mapMgr->CreateGameObject(entry);

	go->CreateFromProto(entry, MapId, x, y, z, o);

	go->SetUInt32Value(GAMEOBJECT_FACTION,faction);
	go->SetFloatValue(OBJECT_FIELD_SCALE_X,scale);	
	go->SetUInt32Value(GAMEOBJECT_FLAGS, flags);
	go->SetFloatValue(GAMEOBJECT_POS_X, x);
	go->SetFloatValue(GAMEOBJECT_POS_Y, y);
	go->SetFloatValue(GAMEOBJECT_POS_Z, z);
	go->SetFloatValue(GAMEOBJECT_FACING, o);
	go->SetInstanceID(m_mapMgr->GetInstanceID());
	go->m_battleground = this;

	return go;
}

void CBattleground::SendChatMessage(uint32 Type, uint64 Guid, const char * Format, ...)
{
	char msg[500];
	va_list ap;
	va_start(ap, Format);
	vsnprintf(msg, 500, Format, ap);
	va_end(ap);
	WorldPacket * data = sChatHandler.FillMessageData(Type, 0, msg, Guid, 0);
	DistributePacketToAll(data);
	delete data;
}

void CBattleground::DistributePacketToAll(WorldPacket * packet)
{
	m_mainLock.Acquire();
	for(int i = 0; i < 2; ++i)
	{
		for(set<Player*>::iterator itr = m_players[i].begin(); itr != m_players[i].end(); ++itr)
			(*itr)->GetSession()->SendPacket(packet);
	}
	m_mainLock.Release();
}

void CBattleground::DistributePacketToTeam(WorldPacket * packet, uint32 Team)
{
	m_mainLock.Acquire();
	for(set<Player*>::iterator itr = m_players[Team].begin(); itr != m_players[Team].end(); ++itr)
		(*itr)->GetSession()->SendPacket(packet);
	m_mainLock.Release();
}

void CBattleground::PlaySoundToAll(uint32 Sound)
{
	WorldPacket data(SMSG_PLAY_SOUND, 4);
	data << Sound;
	DistributePacketToAll(&data);
}

void CBattleground::PlaySoundToTeam(uint32 Team, uint32 Sound)
{
	WorldPacket data(SMSG_PLAY_SOUND, 4);
	data << Sound;
	DistributePacketToTeam(&data, Team);
}

void CBattlegroundManager::SendBattlefieldStatus(Player * plr, uint32 Status, uint32 Type, uint32 InstanceID, uint32 Time, uint32 MapId, uint8 RatedMatch)
{
	WorldPacket data(SMSG_BATTLEFIELD_STATUS, 30);
	if(Status == 0)
		data << uint64(0) << uint32(0);
	else
	{
		if(IS_ARENA(Type))
		{
			//data << uint32(plr->m_bgTeam);
			data << uint32(0);// Queue Slot
			switch(Type)
			{
			case BATTLEGROUND_ARENA_2V2:
				data << uint8(2);
				break;

			case BATTLEGROUND_ARENA_3V3:
				data << uint8(3);
				break;

			case BATTLEGROUND_ARENA_5V5:
				data << uint8(5);
				break;
			}
			data << uint8(0xC);
			data << uint32(6);
			data << uint16(0x1F90);
			data << uint32(11);
			data << uint8(RatedMatch);		// 1 = rated match
		}
		else
		{
			data << uint32(0);
			data << uint8(0) << uint8(2);
			data << Type;
			data << uint16(0x1F90);
			data << InstanceID;
			data << uint8(0);
		}
		
		data << Status;

		switch(Status)
		{
		case 1:					// Waiting in queue
			data << uint32(60) << uint32(0);				// Time / Elapsed time
			break;
		case 2:					// Ready to join!
			data << MapId << Time;
			break;
		case 3:
			if(IS_ARENA(Type))
				data << MapId << uint32(0) << Time << uint8(0);
			else
				data << MapId << uint32(0) << Time << uint8(1);
			break;
		}
	}

	plr->GetSession()->SendPacket(&data);
}

void CBattleground::RemovePlayer(Player * plr, bool logout)
{
	WorldPacket data(SMSG_BATTLEGROUND_PLAYER_LEFT, 30);
	data << plr->GetGUID();

	m_mainLock.Acquire();
	m_players[plr->m_bgTeam].erase(plr);
	if(!plr->m_isGmInvisible)
	{
		DistributePacketToAll(&data);
	}

	memset(&plr->m_bgScore, 0, sizeof(BGScore));
	OnRemovePlayer(plr);
	plr->m_bg = NULL;
	plr->FullHPMP();

	/* are we in the group? */
	if(plr->GetGroup() == m_groups[plr->m_bgTeam])
		plr->GetGroup()->RemovePlayer( plr->m_playerInfo );

	/* reset team */
	plr->ResetTeam();

	/* revive the player if he is dead */
	if(!plr->isAlive() && !logout)
	{
		plr->SetUInt32Value(UNIT_FIELD_HEALTH, plr->GetUInt32Value(UNIT_FIELD_MAXHEALTH));
		plr->ResurrectPlayer();
	}

	/* remove buffs */
	plr->RemoveAura(32727); // Arena preparation
	plr->RemoveAura(44521); // BG preparation
	
	/* teleport out */
	if(!logout)
	{
		if(!IS_INSTANCE(plr->m_bgEntryPointMap))
		{
			LocationVector vec(plr->m_bgEntryPointX, plr->m_bgEntryPointY, plr->m_bgEntryPointZ, plr->m_bgEntryPointO);
			plr->SafeTeleport(plr->m_bgEntryPointMap, plr->m_bgEntryPointInstance, vec);
		}
		else
		{
			LocationVector vec(plr->GetBindPositionX(), plr->GetBindPositionY(), plr->GetBindPositionZ());
			plr->SafeTeleport(plr->GetBindMapId(), 0, vec);
		}

		BattlegroundManager.SendBattlefieldStatus(plr, 0, 0, 0, 0, 0,0);

		/* send some null world states */
		data.Initialize(SMSG_INIT_WORLD_STATES);
		data << uint32(plr->GetMapId()) << uint32(0) << uint32(0);
		plr->GetSession()->SendPacket(&data);
	}

	if(!m_ended && m_players[0].size() == 0 && m_players[1].size() == 0)
	{
		/* create an inactive event */
		sEventMgr.RemoveEvents(this, EVENT_BATTLEGROUND_CLOSE);						// 10mins
		sEventMgr.AddEvent(this, &CBattleground::Close, EVENT_BATTLEGROUND_CLOSE, 600000, 1,0);
	}

	plr->m_bgTeam=plr->GetTeam();
	m_mainLock.Release();
}

void CBattleground::SendPVPData(Player * plr)
{
	m_mainLock.Acquire();
	/*if(m_type >= BATTLEGROUND_ARENA_2V2 && m_type <= BATTLEGROUND_ARENA_5V5)
	{
		m_mainLock.Release();
		return;
	}
	else
	{*/
		WorldPacket data(10*(m_players[0].size()+m_players[1].size())+50);
		BuildPvPUpdateDataPacket(&data);
		plr->GetSession()->SendPacket(&data);
	/*}*/
	
	m_mainLock.Release();
}

void CBattleground::EventCreate()
{
	OnCreate();
}

int32 CBattleground::event_GetInstanceID()
{
	return m_mapMgr->GetInstanceID();
}

void CBattleground::EventCountdown()
{
	if(m_countdownStage == 1)
	{
		m_countdownStage = 2;
		SendChatMessage( CHAT_MSG_BG_EVENT_NEUTRAL, 0, "One minute until the battle for %s begins!", GetName() );
	}
	else if(m_countdownStage == 2)
	{
		m_countdownStage = 3;
		SendChatMessage( CHAT_MSG_BG_EVENT_NEUTRAL, 0, "Thirty seconds until the battle for %s begins!", GetName() );
		sEventMgr.ModifyEventTime(this, EVENT_BATTLEGROUND_COUNTDOWN, 15000);
	}
	else if(m_countdownStage == 3)
	{
		m_countdownStage = 4;
		SendChatMessage( CHAT_MSG_BG_EVENT_NEUTRAL, 0, "Fifteen seconds until the battle for %s begins!", GetName() );
	}
	else
	{
		SendChatMessage( CHAT_MSG_BG_EVENT_NEUTRAL, 0, "The battle for %s has begun!", GetName() );
		sEventMgr.RemoveEvents(this, EVENT_BATTLEGROUND_COUNTDOWN);
		Start();
	}
}

void CBattleground::Start()
{
#ifdef ANTI_CHEAT
	for(uint32 i = 0; i < 2; ++i) {
		for(set<Player*>::iterator itr = m_players[i].begin(); itr != m_players[i].end(); ++itr) {
			if((*itr) && GetStartingCoords((*itr)->GetTeam()). Distance2DSq((*itr)->GetPosition()) > ALLOWED_DISTANCE_AT_START){
				(*itr)->Kick(5000);
				(*itr)->BroadcastMessage("You went too far from the starting place.");
				Anticheat_Log->writefromsession((*itr)->GetSession(), "%s was too far from the starting place at start. BG ID: %u.", (*itr)->GetName(), this->m_id);
			}
		}
	}
#endif
	OnStart();
}

void CBattleground::SetWorldState(uint32 Index, uint32 Value)
{
	map<uint32, uint32>::iterator itr = m_worldStates.find(Index);
	if(itr == m_worldStates.end())
		m_worldStates.insert( make_pair( Index, Value ) );
	else
		itr->second = Value;

	WorldPacket data(SMSG_UPDATE_WORLD_STATE, 8);
	data << Index << Value;
	DistributePacketToAll(&data);
}

void CBattleground::Close()
{
	/* remove all players from the battleground */
	m_mainLock.Acquire();
	m_ended = true;
	for(uint32 i = 0; i < 2; ++i)
	{
		set<Player*>::iterator itr;
		set<uint32>::iterator it2;
		uint32 guid;
		Player * plr;
		for(itr = m_players[i].begin(); itr != m_players[i].end();)
		{
			plr = *itr;
			++itr;
			RemovePlayer(plr, false);
		}
        
		for(it2 = m_pendPlayers[i].begin(); it2 != m_pendPlayers[i].end();)
		{
			guid = *it2;
			++it2;
			plr = objmgr.GetPlayer(guid);

			if(plr)
				RemovePendingPlayer(plr);
			else
				m_pendPlayers[i].erase(guid);
		}
	}

	/* call the virtual onclose for cleanup etc */
	OnClose();

	/* shut down the map thread. this will delete the battleground from the corrent context. */
	m_mapMgr->SetThreadState(THREADSTATE_TERMINATE);

	m_mainLock.Release();
}

Creature * CBattleground::SpawnSpiritGuide(float x, float y, float z, float o, uint32 horde)
{
	if(horde > 1)
		horde = 1;

	CreatureInfo * pInfo = CreatureNameStorage.LookupEntry(13116 + horde);
	if(pInfo == 0)
	{
		return NULL;
	}

	Creature * pCreature = m_mapMgr->CreateCreature(pInfo->Id);

	pCreature->Create(pInfo->Name, m_mapMgr->GetMapId(), x, y, z, o);

	pCreature->SetInstanceID(m_mapMgr->GetInstanceID());
	pCreature->SetUInt32Value(OBJECT_FIELD_ENTRY, 13116 + horde);
	pCreature->SetFloatValue(OBJECT_FIELD_SCALE_X, 1.0f);

	pCreature->SetUInt32Value(UNIT_FIELD_HEALTH, 100000);
	pCreature->SetUInt32Value(UNIT_FIELD_POWER1, 4868);
	pCreature->SetUInt32Value(UNIT_FIELD_POWER3, 200);
	pCreature->SetUInt32Value(UNIT_FIELD_POWER5, 2000000);

	pCreature->SetUInt32Value(UNIT_FIELD_MAXHEALTH, 10000);
	pCreature->SetUInt32Value(UNIT_FIELD_MAXPOWER1, 4868);
	pCreature->SetUInt32Value(UNIT_FIELD_MAXPOWER3, 200);
	pCreature->SetUInt32Value(UNIT_FIELD_MAXPOWER5, 2000000);

	pCreature->SetUInt32Value(UNIT_FIELD_LEVEL, 60);
	pCreature->SetUInt32Value(UNIT_FIELD_FACTIONTEMPLATE, 84 - horde);
	pCreature->SetUInt32Value(UNIT_FIELD_BYTES_0, 0 | (2 << 8) | (1 << 16));

	pCreature->SetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_DISPLAY, 22802);
	pCreature->SetUInt32Value(UNIT_VIRTUAL_ITEM_INFO, 2 | (0xA << 8) | (2 << 16) | (0x11 << 24));
	pCreature->SetUInt32Value(UNIT_VIRTUAL_ITEM_INFO_01, 2);

	pCreature->SetUInt32Value(UNIT_FIELD_FLAGS, UNIT_FLAG_PLUS_MOB | UNIT_FLAG_NOT_ATTACKABLE_9 | UNIT_FLAG_UNKNOWN_10 | UNIT_FLAG_PVP); // 4928

	pCreature->SetUInt32Value(UNIT_FIELD_AURA, 22011);
	pCreature->SetUInt32Value(UNIT_FIELD_AURAFLAGS, 9);
	pCreature->SetUInt32Value(UNIT_FIELD_AURALEVELS, 0x3C);
	pCreature->SetUInt32Value(UNIT_FIELD_AURAAPPLICATIONS, 0xFF);

	pCreature->SetUInt32Value(UNIT_FIELD_BASEATTACKTIME, 2000);
	pCreature->SetUInt32Value(UNIT_FIELD_BASEATTACKTIME_01, 2000);
	pCreature->SetFloatValue(UNIT_FIELD_BOUNDINGRADIUS, 0.208f);
	pCreature->SetFloatValue(UNIT_FIELD_COMBATREACH, 1.5f);

	pCreature->SetUInt32Value(UNIT_FIELD_DISPLAYID, 13337 + horde);
	pCreature->SetUInt32Value(UNIT_FIELD_NATIVEDISPLAYID, 13337 + horde);

	pCreature->SetUInt32Value(UNIT_CHANNEL_SPELL, 22011);
	pCreature->SetUInt32Value(UNIT_MOD_CAST_SPEED, 1065353216);

	pCreature->SetUInt32Value(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_SPIRITHEALER);
	pCreature->SetUInt32Value(UNIT_FIELD_BYTES_2, 1 | (0x10 << 8));

	pCreature->DisableAI();
	pCreature->PushToWorld(m_mapMgr);
	return pCreature;
}

void CBattleground::QueuePlayerForResurrect(Player * plr, Creature * spirit_healer)
{
	m_mainLock.Acquire();
	map<Creature*,set<uint32> >::iterator itr = m_resurrectMap.find(spirit_healer);
	if(itr != m_resurrectMap.end())
		itr->second.insert(plr->GetLowGUID());
	plr->m_areaSpiritHealer_guid=spirit_healer->GetGUID();
	m_mainLock.Release();
}

void CBattleground::RemovePlayerFromResurrect(Player * plr, Creature * spirit_healer)
{
	m_mainLock.Acquire();
	map<Creature*,set<uint32> >::iterator itr = m_resurrectMap.find(spirit_healer);
	if(itr != m_resurrectMap.end())
		itr->second.erase(plr->GetLowGUID());
	plr->m_areaSpiritHealer_guid=0;
	m_mainLock.Release();
}

void CBattleground::AddSpiritGuide(Creature * pCreature)
{
	m_mainLock.Acquire();
	map<Creature*,set<uint32> >::iterator itr = m_resurrectMap.find(pCreature);
	if(itr == m_resurrectMap.end())
	{
		set<uint32> ti;
		m_resurrectMap.insert(make_pair(pCreature,ti));
	}
	m_mainLock.Release();
}

void CBattleground::RemoveSpiritGuide(Creature * pCreature)
{
	m_mainLock.Acquire();
	m_resurrectMap.erase(pCreature);
	m_mainLock.Release();
}

void CBattleground::EventResurrectPlayers()
{
	m_mainLock.Acquire();
	Player * plr;
	set<uint32>::iterator itr;
	map<Creature*,set<uint32> >::iterator i;
	WorldPacket data(50);
	for(i = m_resurrectMap.begin(); i != m_resurrectMap.end(); ++i)
	{
		for(itr = i->second.begin(); itr != i->second.end(); ++itr)
		{
			plr = m_mapMgr->GetPlayer(*itr);
			if(plr && plr->isDead())
			{
                data.Initialize(SMSG_SPELL_START);
				data << plr->GetNewGUID() << plr->GetNewGUID() << uint32(RESURRECT_SPELL) << uint8(0) << uint16(0) << uint32(0) << uint16(2) << plr->GetGUID();
				plr->SendMessageToSet(&data, true);

				data.Initialize(SMSG_SPELL_GO);
				data << plr->GetNewGUID() << plr->GetNewGUID() << uint32(RESURRECT_SPELL) << uint8(0) << uint8(1) << uint8(1) << plr->GetGUID() << uint8(0) << uint16(2)
					<< plr->GetGUID();
				plr->SendMessageToSet(&data, true);

				plr->ResurrectPlayer();
				plr->SetUInt32Value(UNIT_FIELD_HEALTH, plr->GetUInt32Value(UNIT_FIELD_MAXHEALTH));
				plr->SetUInt32Value(UNIT_FIELD_POWER1, plr->GetUInt32Value(UNIT_FIELD_MAXPOWER1));
				plr->SetUInt32Value(UNIT_FIELD_POWER4, plr->GetUInt32Value(UNIT_FIELD_MAXPOWER4));
				plr->CastSpell(plr, BG_REVIVE_PREPARATION, true);
			}
		}
		i->second.clear();
	}
	m_lastResurrect = (uint32)UNIXTIME;
	m_mainLock.Release();
}

void CBattlegroundManager::HandleArenaJoin(WorldSession * m_session, uint32 BattlegroundType, uint8 as_group, uint8 rated_match)
{
	uint32 pguid = m_session->GetPlayer()->GetLowGUID();
	uint32 lgroup = GetLevelGrouping(m_session->GetPlayer()->getLevel());
	if(as_group && m_session->GetPlayer()->GetGroup() == NULL)
		return;

	Group * pGroup = m_session->GetPlayer()->GetGroup();
	if(as_group)
	{
		if(pGroup->GetSubGroupCount() != 1)
		{
			m_session->SystemMessage("Sorry, raid groups joining battlegrounds are currently unsupported.");
			return;
		}
		if(pGroup->GetLeader() != m_session->GetPlayer()->m_playerInfo)
		{
			m_session->SystemMessage("You must be the party leader to add a group to an arena.");
			return;
		}
		if(!pGroup->m_disbandOnNoMembers) // Is a BG/Arena created group, shouldn't be able to queue
			return;

		GroupMembersSet::iterator itx;
		if(!rated_match)
		{
			/* add all players normally.. bleh ;P */
			pGroup->Lock();
			for(itx = pGroup->GetSubGroup(0)->GetGroupMembersBegin(); itx != pGroup->GetSubGroup(0)->GetGroupMembersEnd(); ++itx)
			{
				if((*itx)->m_loggedInPlayer && !(*itx)->m_loggedInPlayer->m_bgIsQueued && !(*itx)->m_loggedInPlayer->m_bg)
					HandleArenaJoin((*itx)->m_loggedInPlayer->GetSession(), BattlegroundType, 0, 0);
			}
			pGroup->Unlock();
			return;
		}
		else
		{
			/* make sure all players are 70 */
			uint32 maxplayers;
			switch(BattlegroundType)
			{
			case BATTLEGROUND_ARENA_2V2:
				maxplayers=2;
				break;

			case BATTLEGROUND_ARENA_3V3:
				maxplayers=3;
				break;

			case BATTLEGROUND_ARENA_5V5:
				maxplayers=5;
				break;

			default:
				maxplayers=2;
				break;
			}

			int32 teamId = -1;
			uint32 arenaTeamType = BattlegroundType-BATTLEGROUND_ARENA_2V2;
			pGroup->Lock();
			for(itx = pGroup->GetSubGroup(0)->GetGroupMembersBegin(); itx != pGroup->GetSubGroup(0)->GetGroupMembersEnd(); ++itx)
			{
				if(maxplayers==0)
				{
					m_session->SystemMessage("You have too many players in your party to join this type of arena.");
					pGroup->Unlock();
					return;
				}

				if((*itx)->lastLevel < 70)
				{
					m_session->SystemMessage("Sorry, some of your party members are not level 70.");
					pGroup->Unlock();
					return;
				}

				if((*itx)->m_loggedInPlayer)
				{
					if((*itx)->m_loggedInPlayer->m_bg || (*itx)->m_loggedInPlayer->m_bg || (*itx)->m_loggedInPlayer->m_bgIsQueued)
					{
						m_session->SystemMessage("One or more of your party members are already queued or inside a battleground.");
						pGroup->Unlock();
						return;
					}
					if(!(*itx)->m_loggedInPlayer->m_arenaTeams[arenaTeamType] || 
						teamId > 0 && teamId != (*itx)->m_loggedInPlayer->m_arenaTeams[arenaTeamType]->m_id)
					{
						m_session->SystemMessage("All party members have to be in the same arena team.");
						pGroup->Unlock();
						return;
					}
					if(teamId < 0)
						teamId = (*itx)->m_loggedInPlayer->m_arenaTeams[arenaTeamType]->m_id;

					--maxplayers;
				}
			}
			if(maxplayers != 0)
			{
				m_session->SystemMessage("You don't have enough players in your party");
				pGroup->Unlock();
				return;
			}
			WorldPacket data(SMSG_GROUP_JOINED_BATTLEGROUND, 4);
			data << uint32(6);		// all arenas

			for(itx = pGroup->GetSubGroup(0)->GetGroupMembersBegin(); itx != pGroup->GetSubGroup(0)->GetGroupMembersEnd(); ++itx)
			{
				if((*itx)->m_loggedInPlayer)
				{
					SendBattlefieldStatus((*itx)->m_loggedInPlayer, 1, BattlegroundType, 0 , 0, 0,1);
					(*itx)->m_loggedInPlayer->m_bgIsQueued = true;
					(*itx)->m_loggedInPlayer->m_bgQueueInstanceId = 0;
					(*itx)->m_loggedInPlayer->m_bgQueueType = BattlegroundType;
					(*itx)->m_loggedInPlayer->GetSession()->SendPacket(&data);
					(*itx)->m_loggedInPlayer->m_bgEntryPointX=(*itx)->m_loggedInPlayer->GetPositionX();
					(*itx)->m_loggedInPlayer->m_bgEntryPointY=(*itx)->m_loggedInPlayer->GetPositionY();
					(*itx)->m_loggedInPlayer->m_bgEntryPointZ=(*itx)->m_loggedInPlayer->GetPositionZ();
					(*itx)->m_loggedInPlayer->m_bgEntryPointMap=(*itx)->m_loggedInPlayer->GetMapId();
				}
			}

			pGroup->Unlock();

			m_queueLock.Acquire();
			m_queuedGroups[BattlegroundType].push_back(pGroup->GetID());
			m_queueLock.Release();
			Log.Success("BattlegroundMgr", "Group %u is now in battleground queue for arena type %u", pGroup->GetID(), BattlegroundType);

			/* send the battleground status packet */

			return;
		}
	}
	

	/* Queue him! */
	m_queueLock.Acquire();
	if(sWorld.BGQueueDisplay)
		sChatHandler.SystemMessage(m_session, "Players in this arena queue: %u", m_queuedPlayersCount[BattlegroundType][lgroup][0]);
	m_queuedPlayers[BattlegroundType][lgroup].push_back(pguid);
	Log.Success("BattlegroundMgr", "Player %u is now in battleground queue for {Arena %u}", m_session->GetPlayer()->GetLowGUID(), BattlegroundType );

	/* send the battleground status packet */
	SendBattlefieldStatus(m_session->GetPlayer(), 1, BattlegroundType, 0 , 0, 0,0);
	m_session->GetPlayer()->m_bgIsQueued = true;
	m_session->GetPlayer()->m_bgQueueInstanceId = 0;
	m_session->GetPlayer()->m_bgQueueType = BattlegroundType;

	/* Set battleground entry point */
	m_session->GetPlayer()->m_bgEntryPointX = m_session->GetPlayer()->GetPositionX();
	m_session->GetPlayer()->m_bgEntryPointY = m_session->GetPlayer()->GetPositionY();
	m_session->GetPlayer()->m_bgEntryPointZ = m_session->GetPlayer()->GetPositionZ();
	m_session->GetPlayer()->m_bgEntryPointMap = m_session->GetPlayer()->GetMapId();
	m_session->GetPlayer()->m_bgEntryPointInstance = m_session->GetPlayer()->GetInstanceID();

	m_queueLock.Release();
}

bool CBattleground::CanPlayerJoin(Player * plr)
{
	return (plr->bGMTagOn || HasFreeSlots(plr->m_bgTeam));
}