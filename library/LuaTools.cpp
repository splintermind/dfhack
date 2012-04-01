﻿/*
https://github.com/peterix/dfhack
Copyright (c) 2009-2011 Petr Mrázek (peterix@gmail.com)

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must
not claim that you wrote the original software. If you use this
software in a product, an acknowledgment in the product documentation
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/

#include "Internal.h"

#include <string>
#include <vector>
#include <map>

#include "MemAccess.h"
#include "Core.h"
#include "VersionInfo.h"
#include "tinythread.h"
// must be last due to MS stupidity
#include "DataDefs.h"
#include "DataIdentity.h"

#include "LuaWrapper.h"
#include "LuaTools.h"

#include "MiscUtils.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

using namespace DFHack;
using namespace DFHack::LuaWrapper;

void DFHack::Lua::PushDFObject(lua_State *state, type_identity *type, void *ptr)
{
    push_object_internal(state, type, ptr, false);
}

void *DFHack::Lua::GetDFObject(lua_State *state, type_identity *type, int val_index, bool exact_type)
{
    return get_object_internal(state, type, val_index, exact_type, false);
}

static int DFHACK_OSTREAM_TOKEN = 0;

color_ostream *DFHack::Lua::GetOutput(lua_State *L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &DFHACK_OSTREAM_TOKEN);
    auto rv = (color_ostream*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return rv;
}

static void set_dfhack_output(lua_State *L, color_ostream *p)
{
    lua_pushlightuserdata(L, p);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DFHACK_OSTREAM_TOKEN);
}

static std::string lua_print_fmt(lua_State *L)
{
    /* Copied from lua source to fully replicate builtin print */
    int n = lua_gettop(L);  /* number of arguments */
    lua_getglobal(L, "tostring");

    std::stringstream ss;

    for (int i=1; i<=n; i++) {
        lua_pushvalue(L, -1);  /* function to be called */
        lua_pushvalue(L, i);   /* value to print */
        lua_call(L, 1, 1);
        const char *s = lua_tostring(L, -1);  /* get result */
        if (s == NULL)
            luaL_error(L, "tostring must return a string to print");
        if (i>1)
            ss << '\t';
        ss << s;
        lua_pop(L, 1);  /* pop result */
    }

    return ss.str();
}

static int lua_dfhack_print(lua_State *S)
{
    std::string str = lua_print_fmt(S);
    if (color_ostream *out = Lua::GetOutput(S))
        *out << str;
    else
        Core::print("%s", str.c_str());
    return 0;
}

static int lua_dfhack_println(lua_State *S)
{
    std::string str = lua_print_fmt(S);
    if (color_ostream *out = Lua::GetOutput(S))
        *out << str << std::endl;
    else
        Core::print("%s\n", str.c_str());
    return 0;
}

static int lua_dfhack_printerr(lua_State *S)
{
    std::string str = lua_print_fmt(S);
    if (color_ostream *out = Lua::GetOutput(S))
        out->printerr("%s\n", str.c_str());
    else
        Core::printerr("%s\n", str.c_str());
    return 0;
}

static int traceback (lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (msg)
        luaL_traceback(L, L, msg, 1);
    else if (!lua_isnoneornil(L, 1)) {  /* is there an error object? */
        if (!luaL_callmeta(L, 1, "__tostring"))  /* try its 'tostring' metamethod */
            lua_pushliteral(L, "(no error message)");
    }
    return 1;
}

static void report_error(color_ostream &out, lua_State *L)
{
    const char *msg = lua_tostring(L, -1);
    if (msg)
        out.printerr("%s\n", msg);
    else
        out.printerr("In Lua::SafeCall: error message is not a string.\n", msg);
    lua_pop(L, 1);
}

bool DFHack::Lua::SafeCall(color_ostream &out, lua_State *L, int nargs, int nres, bool perr)
{
    int base = lua_gettop(L) - nargs;

    color_ostream *cur_out = Lua::GetOutput(L);
    set_dfhack_output(L, &out);

    lua_pushcfunction(L, traceback);
    lua_insert(L, base);

    bool ok = lua_pcall(L, nargs, nres, base) == LUA_OK;

    lua_remove(L, base);
    set_dfhack_output(L, cur_out);

    if (!ok && perr)
        report_error(out, L);

    return ok;
}

bool DFHack::Lua::Require(color_ostream &out, lua_State *state,
                          const std::string &module, bool setglobal)
{
    lua_getglobal(state, "require");
    lua_pushstring(state, module.c_str());

    if (!Lua::SafeCall(out, state, 1, 1))
        return false;

    if (setglobal)
        lua_setglobal(state, module.c_str());
    else
        lua_pop(state, 1);

    return true;
}

static bool load_with_env(color_ostream &out, lua_State *state, const std::string &code, int eidx)
{
    if (luaL_loadbuffer(state, code.data(), code.size(), "=(interactive)") != LUA_OK)
    {
        report_error(out, state);
        return false;
    }

    // Replace _ENV
    lua_pushvalue(state, eidx);

    if (!lua_setupvalue(state, -2, 1))
    {
        out.printerr("No _ENV upvalue.\n");
        return false;
    }

    return true;
}

bool DFHack::Lua::InterpreterLoop(color_ostream &out, lua_State *state,
                                  const char *prompt, int env, const char *hfile)
{
    if (!out.is_console())
        return false;
    if (!lua_checkstack(state, 20))
        return false;

    if (!hfile)
        hfile = "lua.history";
    if (!prompt)
        prompt = "lua";

    DFHack::CommandHistory hist;
    hist.load(hfile);

    out.print("Type quit to exit interactive lua interpreter.\n"
              "Shortcuts:\n"
              " '= foo' => '_1,_2,... = foo'\n"
              " '! foo' => 'print(foo)'\n"
              "Both assign the first result to '_'\n");

    Console &con = static_cast<Console&>(out);

    // Make a proxy global environment.
    lua_newtable(state);
    lua_newtable(state);
    if (env)
        lua_pushvalue(state, env);
    else
        lua_rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);

    // Main interactive loop
    int base = lua_gettop(state);
    int vcnt = 1;
    string curline;
    string prompt_str = "[" + string(prompt) + "]# ";

    for (;;) {
        lua_settop(state, base);

        con.lineedit(prompt_str,curline,hist);

        if (curline.empty())
            continue;
        if (curline == "quit")
            break;

        hist.add(curline);

        char pfix = curline[0];

        if (pfix == '=' || pfix == '!')
        {
            curline = "return " + curline.substr(1);

            if (!load_with_env(out, state, curline, base))
                continue;
            if (!SafeCall(out, state, 0, LUA_MULTRET))
                continue;

            int numret = lua_gettop(state) - base;

            if (numret >= 1)
            {
                lua_pushvalue(state, base+1);
                lua_setfield(state, base, "_");

                if (pfix == '!')
                {
                    lua_pushcfunction(state, lua_dfhack_println);
                    lua_insert(state, base+1);
                    SafeCall(out, state, numret, 0);
                    continue;
                }
            }

            for (int i = 1; i <= numret; i++)
            {
                std::string name = stl_sprintf("_%d", vcnt++);
                lua_pushvalue(state, base + i);
                lua_setfield(state, base, name.c_str());

                out.print("%s = ", name.c_str());

                lua_pushcfunction(state, lua_dfhack_println);
                lua_pushvalue(state, base + i);
                SafeCall(out, state, 1, 0);
            }
        }
        else
        {
            if (!load_with_env(out, state, curline, base))
                continue;
            if (!SafeCall(out, state, 0, LUA_MULTRET))
                continue;
        }
    }

    lua_settop(state, base-1);

    hist.save(hfile);
    return true;
}

static int lua_dfhack_interpreter(lua_State *state)
{
    color_ostream *pstream = Lua::GetOutput(state);
    if (!pstream)
        luaL_error(state, "Cannot use dfhack.interpreter() without output.");

    int argc = lua_gettop(state);

    const char *prompt = (argc >= 1 ? lua_tostring(state, 1) : NULL);
    int env = (argc >= 2 && !lua_isnil(state,2) ? 2 : 0);
    const char *hfile = (argc >= 3 ? lua_tostring(state, 3) : NULL);

    lua_pushboolean(state, Lua::InterpreterLoop(*pstream, state, prompt, env, hfile));
    return 1;
}

static const luaL_Reg dfhack_funcs[] = {
    { "print", lua_dfhack_print },
    { "println", lua_dfhack_println },
    { "printerr", lua_dfhack_printerr },
    { "traceback", traceback },
    { "interpreter", lua_dfhack_interpreter },
    { NULL, NULL }
};

lua_State *DFHack::Lua::Open(color_ostream &out, lua_State *state)
{
    if (!state)
        state = luaL_newstate();

    luaL_openlibs(state);
    AttachDFGlobals(state);

    // Replace the print function of the standard library
    lua_pushcfunction(state, lua_dfhack_println);
    lua_setglobal(state, "print");

    // Create and initialize the dfhack global
    lua_newtable(state);
    luaL_setfuncs(state, dfhack_funcs, 0);
    lua_setglobal(state, "dfhack");

    // load dfhack.lua
    Require(out, state, "dfhack");

    return state;
}

