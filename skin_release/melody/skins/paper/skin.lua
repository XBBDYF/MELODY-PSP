-- Fully replace Home and Player; reuse stock modules elsewhere.
local choices={{"歌单","square"},{"搜索","search"},{"我的","mine"},{"缓存","cache"},{"下载","downloads"},{"皮肤","skins"}}
local selected=1
return {
 draw=function(page,t,dt,m,k)
  if page=="home"then
   ui.rect(0,0,480,272,"#F2EFE3")
   ui.text(18,12,"MELODY",2,"#24231E");ui.text(18,41,"PORTABLE MUSIC FOR YOUR PSP",0,"#7A7567")
   ui.rect(18,61,444,1,"#ADA58F")
   ui.text(20,82,"旧时光。新声音。",2,"#24231E")
   for i,item in ipairs(choices)do local y=131+math.floor((i-1)/3)*43;local x=20+((i-1)%3)*149
    if selected==i then ui.rect(x,y,137,34,"#8C1729")end
    ui.text(x+13,y+8,item[1],1,selected==i and"#FFFFFF"or"#24231E")
   end
   ui.text(22,228,ui.button_hint("←→选择  ○打开  SELECT皮肤中心"),0,"#756F60")
   return true
  elseif page=="player"then
   ui.rect(0,0,480,272,"#F2EFE3");ui.text(20,16,"NOW PLAYING",0,"#8C1729")
   ui.text(20,58,m.song,1,"#24231E");ui.text(20,89,m.artist,0,"#7A7567")
   local angle=t*1.5
   ui.circle(385,145,57,"#8C1729");ui.circle(385,145,28,"#8C1729")
   ui.rect(380+math.floor(math.cos(angle)*30),140+math.floor(math.sin(angle)*30),10,10,"#8C1729")
   ui.text(20,145,string.format("%d:%02d",m.position_ms//60000,(m.position_ms//1000)%60),2,"#8C1729")
   ui.text(20,213,ui.button_hint("○暂停  L/R切歌  ←→进度  ↑↓音量"),0,"#756F60")
   return true -- stock player input retained by returning false below
  end
  return false
 end,
 input=function(page,t,dt,m,k)
  if page~="home"then return false end
  if k.pressed&k.left~=0 then selected=(selected+4)%6+1 end
  if k.pressed&k.right~=0 then selected=selected%6+1 end
  if k.pressed&k.up~=0 or k.pressed&k.down~=0 then selected=(selected+2)%6+1 end
  if k.pressed&k.confirm~=0 then app.dispatch(choices[selected][2])end
  if k.pressed&k.select~=0 then app.dispatch("skins")end
  return true
 end
}
