/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "box_lua_space.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
} /* extern "C" */

#include "space.h"
#include <say.h>

/**
 * Make a single space available in Lua,
 * via box.space[] array.
 *
 * @return A new table representing a space on top of the Lua
 * stack.
 */
static int
lbox_pushspace(struct lua_State *L, struct space *space)
{
	lua_newtable(L);

	/* space.arity */
	lua_pushstring(L, "arity");
	lua_pushnumber(L, space->def.arity);
	lua_settable(L, -3);

	/* space.n */
	lua_pushstring(L, "n");
	lua_pushnumber(L, space_id(space));
	lua_settable(L, -3);

	/* all exists spaces are enabled */
	lua_pushstring(L, "enabled");
	lua_pushboolean(L, 1);
	lua_settable(L, -3);

	/* legacy field */
	lua_pushstring(L, "estimated_rows");
	lua_pushnumber(L, 0);
	lua_settable(L, -3);

	/* space.index */
	lua_pushstring(L, "index");
	lua_newtable(L);
	/*
	 * Fill space.index table with
	 * all defined indexes.
	 */
	for (int i = 0; i <= space->index_id_max; i++) {
		Index *index = space_index(space, i);
		if (index == NULL)
			continue;
		struct key_def *key_def = &index->key_def;
		lua_pushnumber(L, key_def->id);
		lua_newtable(L);		/* space.index[i] */

		lua_pushstring(L, "unique");
		lua_pushboolean(L, key_def->is_unique);
		lua_settable(L, -3);

		lua_pushstring(L, "type");

		lua_pushstring(L, index_type_strs[key_def->type]);
		lua_settable(L, -3);

		lua_pushstring(L, "key_field");
		lua_newtable(L);

		for (uint32_t j = 0; j < key_def->part_count; j++) {
			lua_pushnumber(L, j);
			lua_newtable(L);

			lua_pushstring(L, "type");
			lua_pushstring(L,
			       field_type_strs[key_def->parts[j].type]);
			lua_settable(L, -3);

			lua_pushstring(L, "fieldno");
			lua_pushnumber(L, key_def->parts[j].fieldno);
			lua_settable(L, -3);

			lua_settable(L, -3);
		}

		lua_settable(L, -3);	/* space[i].key_field */

		lua_settable(L, -3);	/* space[i] */
	}

	lua_settable(L, -3);	/* push space.index */

	lua_getfield(L, LUA_GLOBALSINDEX, "box");
	lua_pushstring(L, "bless_space");
	lua_gettable(L, -2);

	lua_pushvalue(L, -3);			/* box, bless, space */
	lua_call(L, 1, 0);
	lua_pop(L, 1);	/* cleanup stack */

	return 1;
}

/** Export a space to Lua */
void
box_lua_space_new(struct lua_State *L, struct space *space)
{
	lua_getfield(L, LUA_GLOBALSINDEX, "box");
	lua_getfield(L, -1, "space");

	if (!lua_istable(L, -1)) {
		lua_pop(L, 1); /* pop nil */
		lua_newtable(L);
		lua_setfield(L, -2, "space");
		lua_getfield(L, -1, "space");
	}

	lbox_pushspace(L, space);
	lua_rawseti(L, -2, space_id(space));

	lua_pop(L, 2); /* box, space */
	assert(lua_gettop(L) == 0);
}

/** Delete a given space in Lua */
void
box_lua_space_delete(struct lua_State *L, struct space *space)
{
	lua_getfield(L, LUA_GLOBALSINDEX, "box");
	lua_getfield(L, -1, "space");

	lua_pushnil(L);
	lua_rawseti(L, -2, space_id(space));
	lua_pop(L, 2); /* box, space */

	assert(lua_gettop(L) == 0);
}
