"""Feedback regressions execute the shipped Lua draw/input functions."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHIM = r'''
local calls,actions={},{}
ui=setmetatable({}, {__index=function(_,key)return function(...)calls[#calls+1]={key,...};return false end end})
ui.text_width=function(s,font)local n=0;for _,cp in utf8.codes(s)do n=n+(cp<128 and 7 or 15)+(font==1 and 2 or 0)end;return n end
app={option=function(_,v)return v end,dispatch=function(a)actions[#actions+1]=a end}
local skin=(function() SOURCE end)()
local m={frame_dt=1/60,song="song",artist="",query="",clock="",cards={},lyric_translation=1}
local k={pressed=0,held=0,up=1,down=2,left=4,right=8,l=16,r=32,back=64,select=128,confirm=256,triangle=512,square=1024}
local function frame(page,t)calls={};assert(skin.draw(page,t or 2,1/60,m,k))end
local function label(s)for _,c in ipairs(calls)do if(c[1]=="label"and c[5]==s)or(c[1]=="lyric_text"and c[5]==s)then return c end end;error("missing "..s)end
local all={}
for i=1,14 do all[i]={index=i,text="line"..i,translation="翻译"..string.rep("长",i%3*14),current=0}end
local function lyrics(cur,scroll,first)
 m.lyrics={};for i=first or math.max(1,cur-3),math.min(14,cur+5)do local r=all[i];r.current=i==cur and 1 or 0;m.lyrics[#m.lyrics+1]=r end;m.lyric_scroll=scroll
end
'''

class ThemeFeedback(unittest.TestCase):
    def run_lua(self, theme, checks):
        lua = shutil.which('lua') or ROOT / '.tmp/live-tile-timing/lua.exe'
        for path in (ROOT / f'tools/skin_templates/{theme}.lua', ROOT / f'skin_release/melody/skins/{theme}/skin.lua'):
            script = SHIM.replace('SOURCE', path.read_text(encoding='utf-8')) + checks
            result = subprocess.run([str(lua), '-'], input=script, encoding='utf-8', capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, str(path) + '\n' + result.stdout + result.stderr)

    def test_adjacent_full_lyric_frames_and_window_shift(self):
        for theme in ('metro', 'xmb'):
            with self.subTest(theme=theme):
                self.run_lua(theme, r'''
lyrics(4,2.98);frame("lyrics");local a=label("line4")
lyrics(5,3.02);frame("lyrics");local b=label("line4")
assert(math.abs(a[3]-b[3])<8,"adjacent frame jumps "..math.abs(a[3]-b[3]))
assert(a[6]==1 and b[6]==1,"old/current font changes geometry")
lyrics(4,2.98,1);frame("lyrics");a=label("line4")
lyrics(7,3.02,4);frame("lyrics");b=label("line4")
assert(math.abs(a[3]-b[3])<8,"fast three-line target clamps scroll")
''')

    def test_metro_half_panel_uses_wrapped_heights(self):
        self.run_lua('metro', r'''
frame("player");k.pressed=k.triangle;skin.input("player",1,1/60,m,k);k.pressed=0
lyrics(4,2.98);frame("player",2);local a=label("line4")
lyrics(5,3.02);frame("player",2+1/60);local b=label("line4")
assert(math.abs(a[3]-b[3])<8,"half panel jumps at line change")
assert(a[6]==1 and b[6]==1)
''')

    def test_home_select_settings(self):
        for theme in ('metro', 'xmb'):
            self.run_lua(theme, 'k.pressed=k.select;k.held=k.select;assert(skin.input("home",1,1/60,m,k));assert(actions[#actions]=="settings")')

    def test_select_l_remains_native(self):
        for theme in ('metro', 'xmb'):
            self.run_lua(theme, 'k.pressed=k.l;k.held=k.select|k.l;assert(skin.input("home",1,1/60,m,k)==false);assert(#actions==0)')

    def test_marked_rows_survive_cursor_move_and_search(self):
        for theme in ('metro', 'xmb'):
            self.run_lua(theme, r'''
m.download_select_mode=1;m.download_selected_count=2;m.list_title="songs";m.search_kind=0;m.search_edit=0
m.songs={{index=1,name="one",artist="a",download_marked=1},{index=2,name="two",artist="b",download_marked=0},{index=3,name="three",artist="c",download_marked=1}}
for _,page in ipairs({"songs","search"})do
 m.selected=2;frame(page);local count=0;local footer=false
 for _,c in ipairs(calls)do if c[1]=="label"and c[5]=="已选"then count=count+1 end;if c[1]=="label"and string.find(c[5],"已选2/32",1,true)then footer=true end end
 assert(count==2,"marked state missing from "..page);assert(footer,"selection count missing")
end
''')

    def test_metro_border_faster_intermediate(self):
        self.run_lua('metro', r'''
frame("home",1);k.pressed=k.right;k.held=k.right;skin.input("home",1,1/60,m,k);frame("home",1+1/60)
local x;for _,c in ipairs(calls)do if c[1]=="rect"and c[6]=="#FFFFFF"and c[5]==2 and c[4]>100 then x=c[2];break end end
assert(x and x>80 and x<238,"border must advance faster without snapping")
''')

if __name__ == '__main__':
    unittest.main()
