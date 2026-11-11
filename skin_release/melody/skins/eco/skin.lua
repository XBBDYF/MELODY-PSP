-- ECO: small draw workload. Hardware policy belongs to C, not per-frame Lua.
local C={bg="#000000",text="#DDDDDD",sub="#AAAAAA",accent="#66CC99"}
local function label(y,s,font,c)ui.label(16,y,448,s or "",font or 0,c or C.text)end
local function draw(t,m)
 ui.rect(0,0,480,272,C.bg)
 label(14,"MELODY  ECO",1,C.accent)
 label(66,((m.song or "")~="")and m.song or "未播放",2)
 label(104,m.artist or "",1,C.sub)
 local mode=(m.power_eco or 0)==1 and "省电"or"标准"
 label(146,mode.." / SoC "..tostring(m.power_effective_clock or 0).." MHz",0,C.accent)
 local timeout=m.power_timeout or 0
 label(168,timeout<0 and"息屏：常亮"or(timeout==0 and"息屏：跟随系统"or("息屏："..tostring(timeout).."秒")),0,C.sub)
 label(238,m.page=="home"and"○ 播放页  △ 搜索  □ 省电开关  SELECT 设置"or"○ 暂停  L/R 切歌  SELECT 菜单",0,C.sub)
end
return {pages={home=draw,player=draw},input=function(page,t,dt,m,k)
 if page~="home"then return false end
 if(k.pressed&k.square)~=0 then app.dispatch("power_eco",(m.power_eco or 0)==1 and"0"or"1");return true end
 if(k.pressed&k.select)~=0 then app.dispatch("settings");return true end
 if(k.pressed&k.triangle)~=0 then app.dispatch("search");return true end
 if(k.pressed&k.confirm)~=0 then app.dispatch("player");return true end
 return false
end}
