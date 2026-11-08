"""Execute the real XMB theme's folder navigation and back layers in Lua."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/xmb-folder-flow';O.mkdir(exist_ok=True)
script=r'''
local dispatched={};local labels={}
ui=setmetatable({label=function(x,y,w,s)labels[#labels+1]=s end,text_width=function(s)return #s*6 end,image=function()return true end,begin_navigation=function()return true end},{__index=function()return function()return false end end})
app={dispatch=function(a,v)dispatched[#dispatched+1]={a,v}end,option=function(id,f)return f end}
local skin=assert(loadfile(arg[1]))();local m={xmb_navigation=1,song="",artist="",paused=1,clock="12:00",battery=90,downloads={},cache={},local_files={},cards={},lyrics={},download_tab=0,cache_tab=0}
local k={up=1,down=2,left=4,right=8,l=16,r=32,confirm=64,back=128,triangle=256,select=512,square=1024,pressed=0,held=0};local time=1
local function draw(page)labels={};assert(skin.draw(page,time,.016,m,k));time=time+.1 end
local function press(page,key)k.pressed=key;k.held=key;assert(skin.input(page,time,.016,m,k));k.pressed=0;k.held=0;time=time+.1 end
-- Music -> recommended folders -> Chinese -> request. No request on folder entry.
draw('home');press('home',k.down);press('home',k.confirm);assert(#dispatched==0);draw('home');assert(table.concat(labels,'|'):find('华语',1,true));press('home',k.down);press('home',k.confirm);assert(dispatched[#dispatched][1]=='xmb_open'and dispatched[#dispatched][2]=='discovery:0');
-- Return goes to previous page through bridge, home retains folder selection.
draw('square');press('square',k.back);assert(dispatched[#dispatched][1]=='xmb_back');draw('home');assert(table.concat(labels,'|'):find('推荐歌单',1,true));local before=#dispatched;press('home',k.back);assert(#dispatched==before);draw('home');assert(table.concat(labels,'|'):find('每日推荐',1,true));
-- Downloads entered elsewhere presents its category folders, then content.
draw('downloads');assert(table.concat(labels,'|'):find('歌曲任务',1,true));assert(table.concat(labels,'|'):find('MV文件',1,true));press('downloads',k.down);press('downloads',k.confirm);assert(dispatched[#dispatched][2]=='downloads:1');m.download_tab=1;draw('downloads');before=#dispatched;press('downloads',k.back);assert(#dispatched==before);draw('downloads');assert(table.concat(labels,'|'):find('MV文件',1,true));press('downloads',k.back);assert(dispatched[#dispatched][1]=='xmb_back');
-- Player has a dedicated lyric route, not Metro's three-stage view.
draw('player');press('player',k.triangle);assert(dispatched[#dispatched][1]=='lyrics');print('PASS real XMB folder-before-content, retained category, one-layer back, downloads tabs as folders, independent player lyrics route')
'''
(O/'test.lua').write_text(script,encoding='utf-8');lua=R/'.tmp/live-tile-timing/lua.exe';assert lua.exists();subprocess.run([lua,O/'test.lua',R/'skin_release/melody/skins/xmb/skin.lua'],check=True)
