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

#ifndef WOWSERVER_DYNAMICOBJECT_H
#define WOWSERVER_DYNAMICOBJECT_H

struct SpellEntry;

typedef set<Unit*>  DynamicObjectList;
typedef set<Unit*>  FactionRangeList;

class SERVER_DECL DynamicObject : public Object
{
public:
	DynamicObject( uint32 high, uint32 low );
	~DynamicObject( );

	void CreateFromGO(GameObject * caster, Spell * pSpell, float x, float y, float z, uint32 duration, float radius);
	void Create(Unit * caster, Spell * pSpell, float x, float y, float z, uint32 duration, float radius);
	void UpdateTargets();

	void AddInRangeObject(Object* pObj);
	void OnRemoveInRangeObject(Object* pObj);
	void Remove();

protected:
	
	SpellEntry * m_spellProto;
	GameObject * g_caster;
	Unit * u_caster;
	Player * p_caster;
	Spell* m_parentSpell;
	DynamicObjectList targets;
	FactionRangeList  m_inRangeOppFactions;
	
	uint32 TypeDynGO;
	uint32 TypeDynUnit;
	uint32 m_aliveDuration;
	float m_radius;
	uint32 _fields[DYNAMICOBJECT_END];
};

#endif

