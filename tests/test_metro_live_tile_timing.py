"""Exercise the actual Metro tile timing/latch in Lua, with source readiness changes."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/live-tile-timing';O.mkdir(exist_ok=True)
s=(R/'skin_release/melody/skins/metro/skin.lua').read_text(encoding='utf-8');s=s.replace('return skin','skin.testTile=tileFace;return skin',1)
prelude=r'''
local calls={};local ready=true
ui=setmetatable({text_width=function(s)return #s*6 end,tile_cover=function(source,index,x,y,w,h)if w>0 then calls[#calls+1]=index end;return ready end},{__index=function()return function()return false end end})
app={dispatch=function()end}
'''
checks=r'''
local skin=assert(loadfile(arg[1]))()
local tile={x=0,y=0,w=218,h=106,name="发现",live="discover"}
local model={cards={{name="one"},{name="two"},{name="three"}},song="",logged_in=1,query=""}
-- i=2 offsets its own clock by3.4sec. Global10.2 is mid-back face.
skin.testTile(tile,2,9.9,model);local before=calls[#calls];skin.testTile(tile,2,10.3,model);assert(calls[#calls]==before,"visible tile changed at unrelated global cycle boundary")
-- A late arriving image must remain text for the rest of this exposed face.
ready=false;skin.testTile(tile,2,14.0,model);skin.testTile(tile,2,18.0,model);local count=#calls
ready=true;skin.testTile(tile,2,18.3,model);assert(#calls==count,"late cover popped onto exposed face")
-- Once another face boundary has passed, a ready image may be latched.
skin.testTile(tile,2,22.3,model);skin.testTile(tile,2,27.5,model);assert(#calls>count)
local action,value;app.dispatch=function(a,v)action=a;value=v end
local k={up=1,down=2,left=4,right=8,l=16,r=32,confirm=64,back=128,square=256,triangle=512,select=1024,pressed=8,held=8}
local grid={grid_selected=2,cards={}};for i=1,30 do grid.cards[i]={id=tostring(i)}end
skin.input("grid",40,.016,grid,k);assert(action=="grid_select"and value=="3","short right must select adjacent card")
k.pressed=0;action=nil;skin.input("grid",40.5,.016,grid,k);assert(action=="grid_select"and value=="8","held right must skip six cards")
k.held=0;k.pressed=32;action=nil;skin.input("grid",41,.016,grid,k);assert(action=="grid_select"and value=="8","grid R jumps a page")
print("PASS actual live tile timing plus short-card/held-page/shoulder-page navigation")
'''
(O/'metro.lua').write_text(s,encoding='utf-8');(O/'test.lua').write_text(prelude+checks,encoding='utf-8')
# The official embedded interpreter is compiled with the same Lua sources.
lua=O/'lua.exe'
if not lua.exists():
 sources=[str(p)for p in(R/'third_party/lua-5.4.7').glob('*.c')if p.name not in ['luac.c','onelua.c','ltests.c']]
 subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-O2',*sources,'-o',lua],check=True)
subprocess.run([lua,O/'test.lua',O/'metro.lua'],check=True)
assert 'selected-stride'in s and 'selected+stride'in s and 'and 1 or 6'in s
print('PASS horizontal D-pad/analogue repeats move a six-card page; native list controls remain native')
