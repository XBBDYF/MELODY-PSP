"""Run the concatenated XMB fragment in real Lua with recording draw helpers."""
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
FRAGMENT = ROOT / "tools/skin_templates/xmb_views.lua"

SHIM = r'''
local C={bg="bg",fg="fg",dim="dim",accent="accent",soft="soft",line="line"}
local calls={}
local function record(kind,...) calls[#calls+1]={kind,...} end
local ui={}
function ui.button_hint(s)return s end
function ui.rect(x,y,w,h,c) assert(w>=0 and h>=0);record("rect",x,y,w,h,c) end
function ui.cover(...) record("cover",...);return false end
function ui.disc(...) record("disc",...) end
function ui.music_fx(...) record("fx",...) end
local function preference(_,fallback)return fallback end
local function text(x,y,w,s,font,col)
 assert(x>=0 and x+w<=480 and y>=0 and y<=256)
 record("text",x,y,w,tostring(s or ""),font,col)
end
local function icon(...)record("icon",...)end
local function background(...)record("background",...)end
local function status(...)record("status",...)end
local function header(...)record("header",...)end
local function bar(...)record("bar",...)end
local function find(kind,fn)
 for _,c in ipairs(calls)do if c[1]==kind and fn(c)then return c end end
 error("missing "..kind)
end
local function hasText(value)
 return find("text",function(c)return c[5]==value end)
end
'''

CHECKS = r'''
local functions={xmbPlayer,xmbSearch,xmbRows,xmbTransfers,xmbOffline}
for _,draw in ipairs(functions)do calls={};draw(1,{}) end
calls={};xmbPlayer(1,{song="Song",artist="Artist",duration_ms=120000,position_ms=30000,disc_player=1})
assert(calls[1][1]=="fx")
find("disc",function(c)return c[2]==40 and c[3]==94 and c[4]==96 end)
find("rect",function(c)return c[2]==40 and c[3]==190 and c[4]==400 end)
hasText("Song");hasText("Artist")
calls={};xmbPlayer(1.1,{song="Song",disc_player=1})
local angle=find("disc",function()return true end)[5]
calls={};xmbPlayer(1.2,{song="Song",paused=1,disc_player=1})
assert(find("disc",function()return true end)[5]==angle);hasText("已暂停")
ui.disc=nil;calls={};xmbPlayer(2,{disc_player=1,buffering=1});hasText("缓冲中")
find("cover",function(c)return c[2]==40 and c[3]==94 and c[4]==96 and c[5]==96 end)
-- Native keyboard coordinates include special cells: row 1 delete, row 2 space/search.
for _,cell in ipairs({{0,0,"q",26,151},{0,9,"p",431,151},{1,9,"←",431,180},{2,7,"空格",341,209},{2,8,"搜索",422,209}})do
 calls={};xmbSearch(0,{search_edit=1,kb_row=cell[1],kb_col=cell[2]})
 local key=hasText(cell[3]);assert(key[2]==cell[4] and key[3]==cell[5])
 local highlights=0
 for _,c in ipairs(calls)do if c[1]=="rect"and c[6]=="soft"then highlights=highlights+1;assert(c[5]==25)end end
 assert(highlights==1)
end
calls={};xmbSearch(0,{search_kind=1,selected=2,cards={{index=1,name="A"},{index=2,name="B"},{index=3,name="C"}}})
assert(hasText("A")[3]==120 and hasText("B")[3]==154 and hasText("C")[3]==188)
assert(hasText("B")[7]=="accent" and hasText("A")[7]=="fg")
calls={};xmbRows(0,{selected=2,songs={total=4,{index=1,name="A"},{index=2,name="B"},{index=3,name="C"},{index=4,name="D"}}})
for i,name in ipairs({"A","B","C","D"})do assert(hasText(name)[3]==90+(i-1)*36)end
for state,label in pairs({[0]="等待中",[1]="下载中",[2]="已完成",[3]="失败",[4]="已暂停"})do
 calls={};xmbTransfers(0,{downloads={{name="Task",state=state,percent=30}}});hasText(label.." · 30%")
end
calls={};xmbTransfers(0,{download_tab=1,downloads={{name="Video",state=2,percent=100}}})
hasText("Video");hasText("已完成 · 100%")
find("header",function(c)return c[2]=="下载 / MV"end)
calls={};xmbOffline(0,{cache_tab=1,local_files={{index=1,name="Local"}},cache={{index=1,name="Cache"}}})
hasText("Local");for _,c in ipairs(calls)do assert(c[1]~="text"or c[5]~="Cache")end
print("PASS five XMB views; keyboard, playback, lists, transfers, offline")
'''


class XmbViews(unittest.TestCase):
    def test_fragment_stays_presentation_only(self):
        source = FRAGMENT.read_text(encoding="utf-8")
        for name in ("xmbPlayer", "xmbSearch", "xmbRows", "xmbTransfers", "xmbOffline"):
            self.assertIn(f"local function {name}(t,m)", source)
        for forbidden in ("playerMode", "previousPlayerMode", "ui.module", "app.dispatch", 'require(', '#FFFFFF'):
            self.assertNotIn(forbidden, source)
        self.assertLess(len(source.encode("utf-8")), 12 * 1024)

    def test_real_lua_fragment(self):
        candidates = [os.environ.get("LUA"), shutil.which("lua"), shutil.which("lua54"),
                      str(ROOT / ".tmp/live-tile-timing/lua.exe")]
        lua = next((path for path in candidates if path and Path(path).is_file()), None)
        if not lua:
            self.fail("Set LUA to a Lua 5.4 interpreter to run the rendering shim")
        script = SHIM + FRAGMENT.read_text(encoding="utf-8") + CHECKS
        result = subprocess.run([lua, "-"], input=script, encoding="utf-8",
                                capture_output=True, cwd=ROOT, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS five XMB views", result.stdout)


if __name__ == "__main__":
    unittest.main()
