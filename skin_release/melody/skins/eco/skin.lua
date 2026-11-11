-- ECO: small draw workload. Hardware policy belongs to C, not per-frame Lua.
local C={bg="#000000",text="#DDDDDD",sub="#AAAAAA",accent="#66CC99"}
local function label(y,s,font,c)ui.label(16,y,448,s or "",font or 0,c or C.text)end
local palettes={
 {bg="#000000",text="#DDDDDD",sub="#AAAAAA",accent="#66CC99"},
 {bg="#EEEDE8",text="#20252B",sub="#5A626D",accent="#2870A0"},
 {bg="#091525",text="#DCEBFF",sub="#91AAC8",accent="#63A6EA"},
 {bg="#201C19",text="#E7DED5",sub="#B5A496",accent="#D49B63"}}
local function draw(t,m)
 C=palettes[(app.option("color_mode")or 0)+1]or palettes[1]
 ui.rect(0,0,480,272,C.bg)
 label(14,"MELODY  ECO",1,C.accent)
 label(66,((m.song or "")~="")and m.song or "未播放",2)
 label(104,m.artist or "",1,C.sub)
 local mode=(m.power_eco or 0)==1 and "省电"or"标准"
 label(146,mode.." / SoC "..tostring(m.power_effective_clock or 0).." MHz",0,C.accent)
 local timeout=m.power_timeout or 0
 label(168,timeout<0 and"息屏：常亮"or(timeout==0 and"息屏：跟随系统"or("息屏："..tostring(timeout).."秒")),0,C.sub)
 label(238,m.page=="home"and ui.button_hint("○ 播放页  △ 搜索  SELECT 设置")or ui.button_hint("○ 暂停  L/R 切歌  SELECT 菜单"),0,C.sub)
end
return {pages={home=draw,player=draw},input=function(page,t,dt,m,k)
 if page~="home"then return false end
 if(k.pressed&k.select)~=0 then app.dispatch("settings");return true end
 if(k.pressed&k.triangle)~=0 then app.dispatch("search");return true end
 if(k.pressed&k.confirm)~=0 then app.dispatch("player");return true end
 return false
end}
