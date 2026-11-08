"""Focused real-Lua / production-preferences Metro palette regression."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SKINS = ROOT / "skin_release/melody/skins"


class MetroColorModeTests(unittest.TestCase):
    def test_metadata_and_live_palette(self):
        meta = json.loads((SKINS / "metro/skin.json").read_text(encoding="utf-8"))
        self.assertEqual(meta["version"], "5.7")
        prefs = {p["id"]: p for p in meta["preferences"]}
        self.assertEqual(prefs["color_mode"]["options"], ["黑色", "白色"])
        self.assertEqual(prefs["color_mode"]["default"], 0)
        self.assertEqual(prefs["live_tiles"]["default"], 1)
        template = (ROOT / "tools/skin_templates/metro.lua").read_text(encoding="utf-8")
        self.assertEqual(template, (SKINS / "metro/skin.lua").read_text(encoding="utf-8"))
        compiler = shutil.which("gcc") or "D:/msys64/ucrt64/bin/gcc.exe"
        runtime = (ROOT / "src/skin_runtime.c").read_text(encoding="utf-8")
        helper = runtime.split("void skin_runtime_refresh_palette(void){", 1)[1].split("\nint skin_runtime_apply", 1)[0]
        with tempfile.TemporaryDirectory(prefix="metro-color-") as temp:
            folder = Path(temp)
            (folder / "app").mkdir()
            source = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "skin_options.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#define SKIN_COLOR_COUNT 15
static unsigned short gfx_rgb(int r,int g,int b){return(r>>3)|((g>>2)<<5)|((b>>3)<<11);}
typedef struct {unsigned short colors[15];unsigned color_mask;int dark;char id[33];} SkinStyle;
static struct {SkinStyle style;}g_vm;
static int color(const char*s,unsigned short*out){unsigned n=0;sscanf(s+1,"%x",&n);*out=(((n>>16)>>3)<<11)|((((n>>8)&255)>>2)<<5)|((n&255)>>3);return 1;}
void io_lock(void){}void io_unlock(void){}void mlog(const char*f,...){(void)f;}
void skin_runtime_refresh_palette(void){HELPER
static int option(lua_State*L){lua_pushinteger(L,skin_options_value(luaL_checkstring(L,1),luaL_optinteger(L,2,0)));return 1;}
static int set_mode(lua_State*L){assert(skin_options_set(0,luaL_checkinteger(L,1))==0);skin_runtime_refresh_palette();return 0;}
int main(int argc,char**argv){assert(argc==3);skin_options_load(argv[1],"metro");strcpy(g_vm.style.id,"metro");g_vm.style.colors[6]=1234;skin_runtime_refresh_palette();assert(g_vm.style.dark&&g_vm.style.colors[6]==1234);
lua_State*L=luaL_newstate();luaL_requiref(L,"_G",luaopen_base,1);lua_pop(L,1);luaL_requiref(L,LUA_TABLIBNAME,luaopen_table,1);lua_pop(L,1);luaL_requiref(L,LUA_STRLIBNAME,luaopen_string,1);lua_pop(L,1);luaL_requiref(L,LUA_MATHLIBNAME,luaopen_math,1);lua_pop(L,1);lua_newtable(L);lua_pushcfunction(L,option);lua_setfield(L,-2,"option");lua_pushcfunction(L,set_mode);lua_setfield(L,-2,"set_mode");lua_setglobal(L,"app");if(luaL_dofile(L,argv[2])){fprintf(stderr,"%s\n",lua_tostring(L,-1));return 1;}lua_close(L);
assert(!g_vm.style.dark&&g_vm.style.colors[6]==1234);unsigned short white; color("#FFFFFF",&white);assert(g_vm.style.colors[0]==white);skin_options_reset();skin_options_load(argv[1],"metro");assert(skin_options_value("color_mode",0)==1);assert(skin_options_value("live_tiles",0)==1);skin_options_load(argv[1],"another");assert(skin_options_value("color_mode",0)==0);strcpy(g_vm.style.id,"metro_custom");g_vm.style.dark=77;skin_runtime_refresh_palette();assert(g_vm.style.dark==77);puts("PASS real Lua black/white draw and input; production prefs persist and isolate; native accent and unrelated styles retained");return 0;}
'''.replace("HELPER", helper)
            (folder / "test.c").write_text(source, encoding="utf-8")
            instrumented = template.replace("return skin\n", "skin.palette=function()return C end\nreturn skin\n")
            (folder / "metro.lua").write_text(instrumented, encoding="utf-8")
            lua_test = r'''
local background
ui=setmetatable({text_width=function(s)return #s*6 end,rect=function(x,y,w,h,c)if x==0 and y==0 and w==480 and h==272 then background=c end end},{__index=function()return function()return false end end})
local skin=assert(loadfile(METRO))()
local m={song="",artist="",query="",clock="12:30",battery=90,volume=50}
local k={pressed=0,held=0,square=64,triangle=128,back=32,l=512,r=1024,select=256,left=4,right=8,up=1,down=2,confirm=16}
assert(skin.draw("home",1,.016,m,k));assert(background=="#080808")
assert(skin.palette().fg=="#FFFFFF")
app.set_mode(1)
skin.input("unknown",1,.016,m,k)
assert(skin.palette().bg=="#FFFFFF" and skin.palette().fg=="#141414")
assert(skin.draw("home",1.1,.016,m,k));assert(background=="#FFFFFF")
assert(skin.palette().accent=="#0078D7")
app.set_mode(0);skin.draw("home",1.2,.016,m,k);assert(background=="#080808")
app.set_mode(1)
'''.replace("METRO", json.dumps((folder / "metro.lua").as_posix()))
            (folder / "test.lua").write_text(lua_test, encoding="utf-8")
            names = (ROOT / "tools/prxloader/lua_sources.txt").read_text(encoding="ascii").splitlines()
            exe = folder / ("palette.exe" if os.name == "nt" else "palette")
            subprocess.run([compiler, "-std=c99", "-O1", "-DSKIN_HOST_TEST", "-DLUA_32BITS",
                            '-DAPP_DIR="' + (folder / "app").as_posix() + '/"',
                            "-I" + str(ROOT / "src"), "-I" + str(ROOT / "third_party/lua-5.4.7"),
                            str(folder / "test.c"), str(ROOT / "src/skin_options.c"),
                            str(ROOT / "src/json.c"), *[str(ROOT / n) for n in names],
                            "-lm", "-o", str(exe)], check=True, timeout=90)
            subprocess.run([str(exe), str(SKINS / "metro"), str(folder / "test.lua")], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
