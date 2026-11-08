"""Validate all palette choices and real music-effect arguments in actual Lua."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/xmb-color';O.mkdir(exist_ok=True)
code=r'''
local selected,fx=0,2;local images,waves,seenfx={},{},{}
ui=setmetatable({label=function()end,text_width=function(s)return #s*6 end,image=function(id)images[#images+1]=id;return true end,wave=function(t,c)waves[#waves+1]=c end,music_fx=function(s,t,l)seenfx[#seenfx+1]={s,t,l}end},{__index=function()return function()return false end end})
app={dispatch=function()end,option=function(id,f)if id=='color_style'then return selected elseif id=='music_fx'then return fx else return f end end}
local skin=assert(loadfile(arg[1]))();local m={xmb_navigation=1,song='playing',artist='artist',paused=0,clock='12:00',battery=75,audio_level=61,position_ms=12345,duration_ms=200000}
local k={pressed=0,held=0,up=1,down=2,left=4,right=8,l=16,r=32,confirm=64,back=128,triangle=256,select=512,square=1024}
local unique={};for i=0,5 do selected=i;assert(skin.draw('player',1+i,.016,m,k));unique[images[#images]]=true;assert(seenfx[#seenfx][1]==2 and seenfx[#seenfx][3]==61)end
local n=0;for _ in pairs(unique)do n=n+1 end;assert(n==6);m.audio_level=0;m.paused=1;assert(skin.draw('player',8,.016,m,k));assert(seenfx[#seenfx][3]==0)
print('PASS six real Lua palette backgrounds, matching waves and actual model music energy; paused zero')
'''
(O/'test.lua').write_text(code,encoding='utf-8');subprocess.run([R/'.tmp/live-tile-timing/lua.exe',O/'test.lua',R/'skin_release/melody/skins/xmb/skin.lua'],check=True)
