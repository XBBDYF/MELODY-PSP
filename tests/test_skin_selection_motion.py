"""Record real Lua frames: selection geometry must travel before settling."""
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHIM = r'''
local skin
local calls={}
ui=setmetatable({}, {__index=function(_,key)return function(...) calls[#calls+1]={key,...};return false end end})
ui.text_width=function(s)return #s*6 end
app={option=function(_,fallback)return fallback end,dispatch=function()end}
local m={frame_dt=.016,song="",artist="",query="",clock="",cards={}}
local function frame(page,t)
 calls={};assert(skin.draw(page,t,.016,m,{}));assert(#calls<=320,"command budget exceeded")
end
local function label(value)
 for _,c in ipairs(calls)do if c[1]=="label"and c[5]==value then return c[3]end end
 error("missing label "..value)
end
local function border()
 for _,c in ipairs(calls)do if c[1]=="rect"and c[6]=="#FFFFFF"and c[5]==2 and c[4]>100 then return c end end
 error("missing border")
end
'''


class SelectionMotion(unittest.TestCase):
    def run_lua(self, name, checks):
        candidates = [os.environ.get("LUA"), shutil.which("lua"),
                      str(ROOT / ".tmp/live-tile-timing/lua.exe")]
        lua = next((p for p in candidates if p and Path(p).is_file()), None)
        self.assertIsNotNone(lua, "Lua interpreter required")
        for folder in ("tools/skin_templates", f"skin_release/melody/skins/{name}"):
            path = ROOT / folder / (name + ".lua" if folder.startswith("tools") else "skin.lua")
            source = path.read_text(encoding="utf-8")
            self.assertLess(len(source.encode()), 48 * 1024)
            script = SHIM + "\nskin=(function()\n" + source + "\nend)()\n" + checks
            result = subprocess.run([lua, "-"], input=script, encoding="utf-8",
                                    capture_output=True, cwd=ROOT, timeout=20)
            self.assertEqual(result.returncode, 0, str(path) + "\n" + result.stdout + result.stderr)

    def test_metro_border_moves_and_changes_width(self):
        self.run_lua("metro", r'''
frame("home",1);local old=border()
local k={pressed=8,held=8,up=1,down=2,left=4,right=8,l=16,r=32,back=64,select=128,confirm=256}
skin.input("home",1.01,.016,m,k)
frame("home",1.016);local mid=border()
assert(mid[2]>old[2]and mid[2]<238,"border snapped horizontally")
assert(mid[4]<old[4]and mid[4]>110,"border width snapped")
local previous=mid[2]
for i=2,50 do frame("home",1+i*.016);local b=border();assert(b[2]>=previous);previous=b[2]end
assert(math.abs(border()[2]-238)<=1)
''')

    def test_metro_grid_pages_slide_both_directions(self):
        self.run_lua("metro", r'''
for i=1,12 do m.cards[i]={index=i,id=tostring(i),name="Card"..i}end
m.grid_selected=1;frame("grid",1)
m.grid_selected=7;frame("grid",1.016);local y=label("Card7")
assert(y>119 and y<191,"next grid page snapped")
for i=2,30 do frame("grid",1+i*.016);local nextY=label("Card7");assert(nextY<=y);y=nextY end
assert(math.abs(y-119)<1)
m.grid_selected=1;frame("grid",1.5);assert(label("Card1")<119,"previous page snapped")
''')

    def test_xmb_grid_list_scrolls_without_jumping(self):
        self.run_lua("xmb", r'''
for i=1,12 do m.cards[i]={index=i,id=tostring(i),name="Card"..i}end
m.grid_selected=1;frame("grid",1);local old=label("Card2")
m.grid_selected=2;frame("grid",1.016);local mid=label("Card2")
assert(mid<old and mid>old-44,"XMB grid snapped")
for i=2,40 do frame("grid",1+i*.016);local y=label("Card2");assert(y<=mid);mid=y end
assert(math.abs(mid-93)<1)
m.frame_dt=nil;ui.begin_navigation=nil;ui.page_fold=nil;ui.capture_page=nil
frame("grid",2)
''')

    def test_xmb_submenu_and_directory_move_between_frames(self):
        self.run_lua("xmb", r'''
frame("home",1);local y=label("正在播放")
local k={pressed=2,held=2,up=1,down=2,left=4,right=8,l=16,r=32,back=64,select=128,confirm=256,triangle=512,square=1024}
skin.input("home",1.01,.016,m,k);frame("home",1.016)
local mid=label("正在播放");assert(mid<y and mid>y-34,"submenu snapped")
for i=2,40 do frame("home",1+i*.016);local nextY=label("正在播放");assert(nextY<=mid);mid=nextY end
k.pressed=256;k.held=256;skin.input("home",2,.016,m,k);frame("home",2)
local old=label("流行");k.pressed=2;k.held=2;skin.input("home",2.01,.016,m,k);frame("home",2.016)
local moved=label("流行");assert(moved<old and moved>old-35,"directory snapped")
''')


if __name__ == "__main__":
    unittest.main()
