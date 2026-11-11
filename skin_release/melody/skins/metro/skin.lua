-- Metro2 / Windows Phone: small, medium and wide Live Tiles plus coordinated pages.
local C={bg="#080808",fg="#FFFFFF",dim="#969696",accent="#0078D7",soft="#171717",line="#3E3E3E"}
local function preference(id,fallback)if app.option then return app.option(id,fallback)end return fallback end
local function refreshPalette()
 if preference("color_mode",0)==1 then
  C.bg="#FFFFFF";C.fg="#141414";C.dim="#666666";C.soft="#EFEFEF";C.line="#D2D2D2"
 else
  C.bg="#080808";C.fg="#FFFFFF";C.dim="#969696";C.soft="#171717";C.line="#3E3E3E"
 end
end
local function tileCover(...)if ui.tile_cover then return ui.tile_cover(...)end return false end
local function avatar(...)if ui.avatar then return ui.avatar(...)end return false end
local inTile=false
local focus,lastPage,entered=1,"",0
local homeSection,visualSection=1,1
local borders={}
local function ease(m)return 1-math.exp(-math.max(0,math.min(.06,m.frame_dt or .016))/.045)end
local function focusBorder(key,x,y,w,h,m)
 local b=borders[key];if not b then b={x=x,y=y,w=w,h=h};borders[key]=b end
 local a=ease(m);b.x=b.x+(x-b.x)*a;b.y=b.y+(y-b.y)*a;b.w=b.w+(w-b.w)*a;b.h=b.h+(h-b.h)*a
 local bx=math.floor(b.x+.5);local by=math.floor(b.y+.5);local bw=math.max(1,math.floor(b.w+.5));local bh=math.max(1,math.floor(b.h+.5))
 ui.rect(bx-2,by-2,bw+4,2,C.fg);ui.rect(bx-2,by+bh,bw+4,2,C.fg);ui.rect(bx-2,by-2,2,bh+4,C.fg);ui.rect(bx+bw,by-2,2,bh+4,C.fg)
end
local sections={"推荐","音乐","我的"}
local repeatKey,repeatAt=0,0
local playerMode,previousPlayerMode,modeChangedAt=0,0,-1
local tileSets={
 {
 {x=16,y=68,w=218,h=106,name="每日推荐",icon="discover",action="daily",live="daily"},
 {x=240,y=68,w=106,h=106,name="发现音乐",icon="music",action="square",live="discover"},
 {x=352,y=68,w=106,h=106,name="搜索",icon="search",action="search",live="search"},
 {x=16,y=180,w=218,h=50,name="正在播放",icon="music",action="player",live="music"},
 {x=240,y=180,w=106,h=50,name="我的音乐",icon="library",section=3},
 {x=352,y=180,w=50,h=50,name="设置",icon="settings",action="settings"},
 {x=408,y=180,w=50,h=50,name="皮肤",icon="skin",action="skins"}},
 {
 {x=16,y=68,w=218,h=106,name="正在播放",icon="music",action="player",live="music"},
 {x=240,y=68,w=106,h=106,name="缓存",icon="library",action="cache",live="library"},
 {x=352,y=68,w=106,h=106,name="本地音乐",icon="library",action="local"},
 {x=16,y=180,w=218,h=50,name="最近播放",icon="music",action="recent"},
 {x=240,y=180,w=106,h=50,name="下载",icon="download",action="downloads",live="download"},
 {x=352,y=180,w=50,h=50,name="搜索",icon="search",action="search"},
 {x=408,y=180,w=50,h=50,name="设置",icon="settings",action="settings"}},
 {
 {x=16,y=68,w=218,h=106,name="我喜欢的音乐",icon="music",action="favorites",live="favorites"},
 {x=240,y=68,w=106,h=106,name="我的歌单",icon="library",action="my_playlists"},
 {x=352,y=68,w=106,h=106,name="账号管理",icon="library",action="account",live="account"},
 {x=16,y=180,w=218,h=50,name="本地与缓存",icon="library",action="cache",live="library"},
 {x=240,y=180,w=106,h=50,name="下载",icon="download",action="downloads",live="download"},
 {x=352,y=180,w=50,h=50,name="设置",icon="settings",action="settings"},
 {x=408,y=180,w=50,h=50,name="皮肤",icon="skin",action="skins"}}}
local tiles=tileSets[1]
local function text(x,y,w,s,font,col)ui.label(x,y,w,tostring(s or""),font or 0,col or(inTile and"#FFFFFF"or C.fg))end
local function line(x,y,x2,y2,col,width)ui.line(x,y,x2,y2,col or C.fg,width or 1)end
local function sound(id)if ui.sound then ui.sound(id)end end
local function icon(kind,x,y,s,col)
 col=col or(inTile and"#FFFFFF"or C.fg)
 if kind=="music"then
  line(x+s*.37,y+s*.12,x+s*.37,y+s*.73,col,3);line(x+s*.37,y+s*.12,x+s*.82,y,col,3);line(x+s*.82,y,x+s*.82,y+s*.59,col,3)
  ui.circle(x+s*.24,y+s*.78,s*.14,col);ui.circle(x+s*.68,y+s*.65,s*.14,col)
 elseif kind=="search"then ui.circle(x+s*.4,y+s*.36,s*.28,col);line(x+s*.6,y+s*.59,x+s*.91,y+s*.9,col,3)
 elseif kind=="library"then for i=0,2 do line(x+i*s*.27,y+s*.12,x+i*s*.27,y+s*.87,col,3)end;line(x+s*.75,y+s*.08,x+s*.95,y+s*.82,col,3)
 elseif kind=="download"then
  line(x+s*.48,y,x+s*.48,y+s*.62,col,3);line(x+s*.2,y+s*.38,x+s*.48,y+s*.65,col,3);line(x+s*.76,y+s*.38,x+s*.48,y+s*.65,col,3)
  line(x+s*.08,y+s*.77,x+s*.08,y+s*.94,col,2);line(x+s*.08,y+s*.94,x+s*.9,y+s*.94,col,2);line(x+s*.9,y+s*.94,x+s*.9,y+s*.77,col,2)
 elseif kind=="settings"then ui.circle(x+s*.5,y+s*.5,s*.27,col);ui.circle(x+s*.5,y+s*.5,s*.1,col);for i=0,7 do local a=i*math.pi/4;line(x+s*.5+math.cos(a)*s*.29,y+s*.5+math.sin(a)*s*.29,x+s*.5+math.cos(a)*s*.43,y+s*.5+math.sin(a)*s*.43,col,3)end
 elseif kind=="skin"then for i=0,3 do ui.rect(x+(i%2)*s*.51,y+(i//2)*s*.51,s*.37,s*.37,col)end
 elseif kind=="discover"then ui.circle(x+s*.5,y+s*.5,s*.43,col);line(x+s*.3,y+s*.7,x+s*.65,y+s*.25,col,2);line(x+s*.65,y+s*.25,x+s*.56,y+s*.56,col,2)
 elseif kind=="play"then line(x+s*.2,y,x+s*.88,y+s*.5,col,2);line(x+s*.88,y+s*.5,x+s*.2,y+s,col,2);line(x+s*.2,y+s,x+s*.2,y,col,2)
 elseif kind=="pause"then ui.rect(x+s*.19,y,s*.18,s,col);ui.rect(x+s*.61,y,s*.18,s,col)
 elseif kind=="prev"or kind=="next"then local d=kind=="next"and 1 or-1;local cx=x+s*.5;line(cx-d*s*.35,y,cx+d*s*.25,y+s*.5,col,2);line(cx+d*s*.25,y+s*.5,cx-d*s*.35,y+s,col,2);line(cx+d*s*.4,y,cx+d*s*.4,y+s,col,2)end
end
local function sizeText(bytes)
 bytes=math.max(0,bytes or 0);if bytes>=1048576 then return string.format("%.1f MB",bytes/1048576)elseif bytes>=1024 then return string.format("%.0f KB",bytes/1024)else return tostring(bytes).." B"end
end
local function header(title,sub,m)
 text(16,5,70,"MELODY",0,C.dim)
 local status=(m.clock or"").."  "..tostring(m.battery or 0).."%"
 if m.show_speed==1 then status=tostring(m.network_kbps or 0).."KB/s  "..status end
 if m.show_memory==1 then status=string.format("%.1f/%.0fM",(m.memory_used_kb or 0)/1024,((m.memory_used_kb or 0)+(m.memory_free_kb or 0))/1024).."  "..status end
 local width=ui.text_width(status,0);text(math.max(95,464-width),5,math.min(369,width+1),status,0,C.dim)
 text(15,22,445,title,2);if sub then text(20,53,438,sub,0,C.dim)end
end
local extraHints={home="△回歌曲详情 · SELECT设置 · L/R分类",player="SELECT菜单  □喜欢  ↑↓音量  ×列表",lyrics="←→进度  ↑↓音量  △返回播放器",search="L/R搜索分类 · SELECT多选下载 · ←→快移",settings="△账号与登录 · SELECT+L皮肤中心",mine="△回歌曲详情 · SELECT+L皮肤中心",downloads="进度来自下载队列 · SELECT+L皮肤中心",cache="缓存/本地分别选曲 · SELECT+L皮肤中心",grid="封面加载后显示 · SELECT+L皮肤中心",square="封面加载后显示 · SELECT+L皮肤中心"}
local function bar(left,right)ui.rect(0,236,480,36,C.soft);text(16,239,325,ui.button_hint(left),0,C.dim);text(354,239,110,ui.button_hint(right or""),0,C.dim);text(16,255,448,ui.button_hint((lastPage=="player"and playerMode and playerMode~=0)and"SELECT菜单  □译文  ↑↓音量  ×列表"or extraHints[lastPage]or"SELECT+L皮肤中心"),0,C.dim)end
local function row(y,label,value,selected,tag)
 if selected then ui.rect(15,y+2,3,27,C.accent)end;text(26,y+1,tag and 386 or 420,label,1,selected and C.accent or C.fg)
 if tag then text(438,y+1,44,tag,0,C.accent)end
 if value and value~=""then text(27,y+20,410,value,0,C.dim)end
end
local function songRow(y,s,selected,m)
 if m.download_select_mode~=1 then row(y,s.name,s.artist,selected,(s.fee or 0)<0 and"酷我"or(s.playable==2 and"三方"or nil));return end
 local marked=s.download_marked==1;local col=marked and C.accent or C.dim
 if selected then text(22,y,18,">",0,C.accent)end
 if marked then ui.rect(15,y,5,33,C.accent)end
 ui.rect(43,y+3,18,18,col);ui.rect(45,y+5,14,14,marked and C.accent or C.bg)
 if marked then line(46,y+12,50,y+16,C.bg,2);line(50,y+16,58,y+7,C.bg,2);text(413,y+3,48,"已选",0,C.accent)end
 text(70,y,337,s.name,0,selected and C.accent or C.fg);text(70,y+17,337,s.artist,0,C.dim)
end
local function selectionHint(m)
 return "已选"..tostring(m.download_selected_count or 0).."/32  □选/取消  △下载"
end
local function repeatDirection(t,k)
 local mask=k.up|k.down|k.left|k.right;local down=k.held&mask;local pressed=k.pressed&mask
 if pressed~=0 then repeatKey=down;repeatAt=t+.42;return pressed end
 if down==0 then repeatKey=0;return 0 end
 if down~=repeatKey then repeatKey=down;repeatAt=t+.42;return 0 end
 if t>=repeatAt then repeatAt=t+.15;return down end;return 0
end
local function neighbor(direction)
 local a=tiles[focus];local ax=a.x+a.w/2;local ay=a.y+a.h/2;local best=nil;local score=1e20
 for i,b in ipairs(tiles)do if i~=focus then local dx=b.x+b.w/2-ax;local dy=b.y+b.h/2-ay;local ahead=direction=="left"and-dx or direction=="right"and dx or direction=="up"and-dy or dy
  if ahead>1 then local side=(direction=="left"or direction=="right")and math.abs(dy)or math.abs(dx);local cost=ahead+side*2.8;if cost<score then best=i;score=cost end end
 end end
 if best then focus=best;sound("move")
 elseif direction=="right" and homeSection<3 then homeSection=homeSection+1;tiles=tileSets[homeSection];focus=1;sound("move")
 elseif direction=="left" and homeSection>1 then homeSection=homeSection-1;tiles=tileSets[homeSection];focus=3;sound("move")end
end
local tileFaces={}
local function tileFace(b,i,t,m)
 local clock=t+i*1.7;local phase=clock%10.2;local cycle=math.floor(clock/10.2);local back=phase>=.13 and phase<5.23;local amount=1
 local key=b.live or b.name;local face=tileFaces[key]
 local coverIndex=#(m.cards or{})>0 and cycle%#m.cards+1 or 1
 local source=b.live=="daily"and"daily"or b.live=="discover"and"recommendations"or nil
 -- Queue the correct thumbnail even while its text/front face is visible.
 local ready=source and tileCover(source,b.live=="daily"and 1 or coverIndex,0,0,0,0)or false
 if not face or face.back~=back then
  face={back=back,index=coverIndex,ready=ready,title=b.live=="daily"and m.daily_song or((m.cards or{})[coverIndex]or{}).name};tileFaces[key]=face
 end
 if phase<.26 then amount=math.abs(math.cos(phase/.26*math.pi))elseif phase>=5.1 and phase<5.36 then amount=math.abs(math.cos((phase-5.1)/.26*math.pi))end
 if not b.live or preference("live_tiles",1)==0 then amount=1;back=false end
 local h=math.max(2,math.floor(b.h*amount));local y=b.y+(b.h-h)/2

 ui.rect(b.x,y,b.w,h,i==2 and"#008A82"or i==4 and"#125EA5"or C.accent)
 if h<b.h*.65 then return end
 local picture=false
 if not back and b.live=="music"and m.song~=""then picture=ui.cover(b.x,y,b.w,h)
 elseif b.live=="account"then picture=avatar(b.x,y,b.w,h)
 elseif back and b.live=="daily"then
  picture=face.ready and tileCover("daily",1,b.x,y,b.w,h)
 elseif back and b.live=="discover"and #(m.cards or{})>0 then
  picture=face.ready and tileCover("recommendations",face.index,b.x,y,b.w,h)
 end
 if picture then
  local title=b.live=="music"and m.song or b.live=="daily"and m.daily_song or b.live=="account"and(m.login_name~=""and m.login_name or"账号管理")or b.name
  if b.live=="discover"or b.live=="daily"then title=face.title or b.name end
  ui.overlay(b.x,y+h-29,b.w,29,"#000000",175);text(b.x+10,y+h-24,b.w-20,title,0,"#FFFFFF");return end
 if back then
  if b.live=="daily"then text(b.x+12,y+13,b.w-24,"为今天挑选的新声音",1);text(b.x+12,y+43,b.w-24,m.logged_in==1 and"专属每日推荐"or"登录后获取你的每日推荐",0);text(b.x+12,y+h-24,b.w-24,ui.button_hint("○ 打开 · 每日更新"),0)
  elseif b.live=="favorites"then text(b.x+12,y+15,b.w-24,tostring(m.favorite_count or 0).." 首本机收藏",1);text(b.x+12,y+46,b.w-24,m.logged_in==1 and"云端喜欢已连接"or"登录后同步云端喜欢",0)
  elseif b.live=="music"then
   if b.h<70 then text(b.x+12,y+8,b.w-24,m.song~=""and m.song or"音乐，随身而行",0);text(b.x+12,y+28,b.w-24,m.artist~=""and m.artist or ui.button_hint("○ 打开播放器"),0)
   else text(b.x+12,y+12,b.w-24,m.song~=""and m.song or"音乐，随身而行",1);text(b.x+12,y+38,b.w-24,m.artist~=""and m.artist or"打开歌曲，发现下一首",0);text(b.x+12,y+h-24,b.w-24,m.song~=""and(m.paused==1 and"已暂停"or"正在播放")or ui.button_hint("○ 打开播放器"),0)end
  elseif b.live=="library"then text(b.x+10,y+10,b.w-20,tostring(m.cache_count or 0),3);text(b.x+10,y+55,b.w-20,"首已缓存",0);text(b.x+10,y+78,b.w-20,tostring(m.local_count or 0).." 首本地",0)
  elseif b.live=="search"then text(b.x+10,y+12,b.w-20,m.query~=""and m.query or"想听什么？",1);text(b.x+10,y+56,b.w-20,m.online==1 and"在线搜索"or"连接后搜索",0);text(b.x+10,y+78,b.w-20,"歌曲 · 歌手",0)
  elseif b.live=="discover"then text(b.x+12,y+8,b.w-24,"歌单 / 排行榜 / 新声音",0);text(b.x+12,y+28,b.w-24,"发现下一首喜欢的歌",0)
  elseif b.live=="account"then text(b.x+10,y+12,b.w-20,m.logged_in==1 and(m.login_name~=""and m.login_name or"已登录")or"登录账号",1);text(b.x+10,y+56,b.w-20,"管理与同步",0)
  elseif b.live=="download"then text(b.x+10,y+8,b.w-20,tostring(m.download_count or 0).." 项",1);text(b.x+10,y+29,b.w-20,"后台下载",0)end
 else local size=b.w<70 and 24 or b.h<70 and 25 or 43;local x=b.w<70 and b.x+(b.w-size)/2 or b.h<70 and b.x+12 or b.x+(b.w-size)/2;icon(b.icon,x,b.h<70 and y+11 or y+16,size)
  if b.w>=70 then text(b.h<70 and b.x+48 or b.x+10,b.h<70 and y+16 or y+h-25,b.w-(b.h<70 and 54 or 20),b.name,b.h<70 and 0 or 1)end
 end
end
local function home(t,m)
 header("开始",nil,m)
 for i,label in ipairs(sections)do text(100+(i-1)*85,29,80,label,1,i==homeSection and C.fg or C.dim)end
 local dt=math.max(0,math.min(.06,m.frame_dt or .016));visualSection=visualSection+(homeSection-visualSection)*math.min(1,dt*14)
 local shift=math.max(-440,math.min(440,math.floor((homeSection-visualSection)*440)))
 tiles=tileSets[homeSection]
 for i,b in ipairs(tiles)do inTile=true;tileFace(b,i,t,m);inTile=false end
 local b=tiles[focus];focusBorder("home",b.x,b.y,b.w,b.h,m)
 if shift~=0 then ui.slide(shift,64,239,C.bg)end
 bar(sections[homeSection].." · "..tiles[focus].name.."  ○打开","L/R 分类  ×返回")
end
local mineOrder={8,1,3,2,4,6,5,7,9,10,12,11}
local function mine(t,m)
 header("我的",nil,m)
 local accountChosen=(m.mine_selected or 1)==8
 ui.rect(16,64,448,62,accountChosen and C.accent or C.soft)
 if not avatar(24,71,48,48)then ui.rect(24,71,48,48,accountChosen and"#0065B5"or C.accent);icon("library",36,83,24,"#FFFFFF")end
 text(85,72,350,m.logged_in==1 and(m.login_name~=""and m.login_name or"已登录账号")or"登录网易云音乐",1,accountChosen and"#FFFFFF"or C.fg)
 text(85,98,350,"账号管理 · 登录 / 退出 / 刷新资料",0,accountChosen and"#FFFFFF"or C.dim)
 local chosen=2;for i,index in ipairs(mineOrder)do if index==m.mine_selected then chosen=i end end
 local first=math.max(2,chosen-1);local shown=0
 for slot=first,math.min(#mineOrder,first+2)do local index=mineOrder[slot];local r=(m.mine or{})[index];if r then
  local value="";if index==4 then value=tostring(m.local_count or 0).." 首本地"elseif index==5 then value=tostring(m.download_count or 0).." 项任务"elseif index==6 then value=tostring(m.cache_count or 0).." 首缓存"end
  row(137+shown*31,r.label,value,index==m.mine_selected);shown=shown+1
 end end
 bar("↑↓选择  ○账号/功能  △回播放","L/R主菜单  ×开始")
end
local function settings(t,m)
 header("设置","系统  /  个性化",m);local selected=m.setting_selected or 1;local first=math.max(1,selected-2);local shown=0
 for _,r in ipairs(m.settings or{})do if r.index>=first and shown<4 then row(78+shown*39,r.label,r.value,r.index==selected);shown=shown+1 end end
 text(425,224,40,tostring(selected).."/"..tostring((m.settings or{}).total or 24),0,C.dim);bar("↑↓选择  ←→修改  ○进入","× 我的")
end
local function wrapLines(value,width,font)
 local lines,line={},""
 for _,cp in utf8.codes(value or"")do local ch=utf8.char(cp);if ch=="\n"then lines[#lines+1]=line;line=""elseif ui.text_width(line..ch,font)>width and line~=""then lines[#lines+1]=line;line=ch else line=line..ch end end
 if line~=""or#lines==0 then lines[#lines+1]=line end;return lines
end
local function drawLyrics(x,width,m,anchor,top,bottom)
 local rows=m.lyrics or{};local current=1;local layouts={}
 for i,r in ipairs(rows)do
  if r.current==1 then current=i end
  local original=wrapLines(r.text,width,1);local translation=m.lyric_translation==1 and(r.translation or"")~=""and wrapLines(r.translation,width,0)or{}
  layouts[i]={original=original,translation=translation,height=#original*21+#translation*17+12}
 end
 if#rows==0 then text(x,anchor,width,"歌词加载中或暂无歌词",0,C.dim);return end
 -- Integrate the absolute fractional index, independently of the current row
 -- and of the prefix removed when the native lyric window advances.
 local position=m.lyric_scroll or(rows[current].index-1);local offset=0
 for i,r in ipairs(rows)do local start=r.index-1
  if position>=start+1 then offset=offset+layouts[i].height
  elseif position>=start then offset=offset+(position-start)*layouts[i].height;break
  else if i==1 then offset=(position-start)*layouts[i].height end;break end
 end
 local y=anchor-offset
 for i,r in ipairs(rows)do local l=layouts[i];if y+l.height>=top and y<bottom then local py=y;local bytes=0
  for _,chunk in ipairs(l.original)do
   if i==current and ui.lyric_text then ui.lyric_text(x,py,width,chunk,1,C.dim,C.accent,r.index,bytes)else text(x,py,width,chunk,1,i==current and C.accent or C.dim)end
   bytes=bytes+#chunk;py=py+21
  end
  for _,chunk in ipairs(l.translation)do text(x,py,width,chunk,0,i==current and C.fg or C.dim);py=py+17 end
 end;y=y+l.height end
end
local function lyricText(x,y,w,value,font,base,active,index,offset)
 if ui.lyric_text and index then ui.lyric_text(x,y,w,value,font,base,active,index,offset or 0)else text(x,y,w,value,font,active)end
end
local function lyricPanel(x,width,m)
 drawLyrics(x+8,width-18,m,112,74,226)
end
local function detailPanel(x,t,m)
 text(x,70,275,m.song~=""and m.song or"还没有播放歌曲",1);text(x,99,275,m.artist~=""and m.artist or"从歌单、本地或缓存开始",0,C.dim);text(x,123,270,(m.quality or"").."  "..tostring(m.volume or 0).."%",0,C.dim)
 if m.cache_percent and m.cache_percent>=0 and m.cache_percent<100 then text(x,141,270,(m.buffering==1 and"缓冲中 "or"缓存 ")..tostring(m.cache_percent).."%",0,C.dim)end
 local dur=math.max(1,m.duration_ms or 0);local pos=m.position_ms or 0;ui.rect(x,170,271,3,C.line)
 if m.cache_percent and m.cache_percent>=0 and m.cache_percent<100 then ui.rect(x,170,271*math.min(100,m.cache_percent)/100,3,"#777777")end
 ui.rect(x,170,math.min(271,271*pos/dur),3,C.accent)
 text(x,178,120,string.format("%d:%02d",pos//60000,(pos//1000)%60),0,C.dim);text(x+214,178,60,string.format("%d:%02d",dur//60000,(dur//1000)%60),0,C.dim)
 icon("prev",x+26,206,18);icon(m.paused==1 and"play"or"pause",x+116,202,23);icon("next",x+218,206,18);end
local discAngle,discTime=0,0
local function player(t,m)
 header(playerMode==0 and"正在播放"or playerMode==1 and"歌词与封面"or"歌词",nil,m)
 local p=math.max(0,math.min(1,(t-modeChangedAt)/.25));p=1-(1-p)^3
 local mode=playerMode
 local discDt=math.max(0,math.min(.1,t-discTime));discTime=t;if m.paused~=1 and m.song~=""then discAngle=(discAngle+discDt*18)%360 end
 local coverX=17
 if mode==2 then coverX=17-190*p elseif previousPlayerMode==2 and p<1 then coverX=-173+190*p end
 if mode~=2 or p<1 then
  ui.clip(0,64,480,234);if coverX>=0 and coverX+154<=480 then if m.disc_player==1 and ui.disc then ui.disc(coverX,67,154,discAngle)elseif not ui.cover(coverX,67,154,154)then ui.rect(coverX,67,154,154,C.accent)end else if not ui.cover(coverX,67,154,154)then ui.rect(coverX,67,154,154,C.accent)end end;ui.unclip()
 end
 if p<1 and previousPlayerMode~=mode then
  if previousPlayerMode==0 then ui.clip(175,64,480,234);detailPanel(188-300*p,t,m);ui.unclip()
  elseif previousPlayerMode==1 then ui.clip(175,64,480,234);lyricPanel(181-300*p,283,m);ui.unclip()
  else ui.clip(0,64,480,234);lyricPanel(17-480*p,446,m);ui.unclip()end
 end
 if mode==0 then ui.clip(175,64,480,234);detailPanel(188+300*(1-p),t,m);ui.unclip()
 elseif mode==1 then ui.clip(175,64,480,234);lyricPanel(181+300*(1-p),283,m);ui.unclip()
 else ui.clip(0,64,480,234);lyricPanel(17+480*(1-p),446,m);ui.unclip()end
 bar("○暂停  L/R切歌  ←→进度",mode==0 and"△封面歌词"or mode==1 and"△全屏歌词"or"△歌曲详情")
end
local function songs(t,m)
 header(m.list_title~=""and m.list_title or"音乐",tostring((m.songs or{}).total or 0).." 首歌曲",m);local first=math.max(1,(m.selected or 1)-2);local shown=0
 for _,s in ipairs(m.songs or{})do if s.index>=first and shown<4 then songRow(78+shown*39,s,s.index==m.selected,m);shown=shown+1 end end;bar(m.download_select_mode==1 and selectionHint(m)or"○播放  △下载  L多选下载","←→快移  ×返回")
end
local function search(t,m)
 header("搜索",nil,m);ui.rect(17,63,444,30,"#FFFFFF");icon("search",26,70,16,"#151515");local pre=(m.ime_active or 0)==1 and(m.ime_pre or"")or"";if(m.query or"")~=""then text(51,70,330,m.query,0,"#333333")elseif pre==""then text(51,70,330,"歌曲、歌手、歌单",0,"#333333")end;if pre~=""then local qw=ui.text_width(m.query or"",1);text(51+math.min(qw,300),70,300,pre,0,C.accent)end;if m.search_edit==1 then text(400,70,56,(m.ime_mode or 1)==2 and"假名"or((m.ime_mode or 1)==3 and"英文"or"拼音"),0,C.accent)end
 if m.search_edit==1 then
  local cands=m.ime_candidates or{};local hist=m.search_history or{};local recs=m.search_chips or{};local assoc=m.assoc or{}
  local ime=(m.ime_active or 0)==1
  local top=assoc;local topLabel="联想"
  if #top==0 then local sgs=m.suggestions or{};if (not ime)and#sgs>0 then top={};for i=1,math.min(#sgs,6)do top[i]=sgs[i]end;topLabel="联想" elseif (not ime)then top=recs;topLabel="推荐" end end
  local toff=m.chip_off or 0;if toff>#top-1 then toff=math.max(0,#top-3)end
  if #top>0 then
   text(17,102,26,topLabel,0,C.dim)
   if toff>0 then text(45,102,8,"<",0,C.accent)end
   for i=1,3 do local idx=toff+i;if top[idx]then local x=56+(i-1)*96;local sel=m.chip_row==1 and (m.chip_selected or 1)==idx;if sel then ui.rect(x,99,92,20,C.accent)end;text(x+4,102,86,top[idx],0,sel and C.bg or C.fg)end end
   if toff+3<#top then text(344,102,8,">",0,C.accent)end
  end
  if ime and #cands>0 then
   local off=m.ime_off or 0;text(17,122,26,"候选",0,C.dim)
   if off>0 then text(45,122,8,"<",0,C.accent)end
   for i=0,3 do local idx=off+i+1;local v=cands[i+1];if v and v~=""then local x=56+i*68;local sel=m.chip_row==2 and (m.ime_selected or 1)==idx;if sel then ui.rect(x,119,64,20,C.accent)end;text(x+3,122,58,v,0,sel and C.bg or C.fg)end end
   if off+4<(m.ime_total or #cands)then text(330,122,8,">",0,C.accent)end
  end
  text(356,101,110,"历史",0,C.dim)
  for i=1,math.min(#hist,2)do local y=115+(i-1)*16;local sel=m.chip_row==3 and (m.hist_selected or 1)==i;if sel then ui.rect(353,y-1,116,15,C.accent)end;text(357,y,110,hist[i],0,sel and C.bg or C.dim)end
  local letters={"qwertyuiop","asdfghjkl","zxcvbnm"};for r=0,2 do for col=0,(r<2 and 9 or 8)do
   local label=col<#letters[r+1]and string.sub(letters[r+1],col+1,col+1)or r==1 and"←"or col==7 and"空格"or"搜索"
   local x=17+col*45;local w=41;if r==2 and col==7 then w=76 elseif r==2 and col==8 then x=413;w=48 end
   local y=145+r*29;if m.kb_row==r and m.kb_col==col and m.suggest_focused~=1 then ui.rect(x,y,w,25,C.accent)else ui.rect(x,y,w,25,C.soft)end;text(x+9,y+6,w-11,label,0,C.fg)
  end end
  bar("○ 上屏  △ 选词  □ 切换语言","× 返回");return end
 local categories={"歌曲","歌单","专辑","歌手","声音"};for i,v in ipairs(categories)do text(18+(i-1)*88,103,82,v,0,i-1==m.search_kind and C.accent or C.dim)end
 if m.search_state==1 then -- native bottom loading line is the only wait indicator

 else local first=math.max(1,(m.selected or 1)-1);local shown=0;for _,s in ipairs(m.search_kind==0 and(m.songs or{})or(m.cards or{}))do if s.index>=first and shown<3 then if(m.search_kind or 0)==0 then songRow(130+shown*33,s,s.index==m.selected,m)else row(130+shown*33,s.name,s.subtitle,s.index==m.selected)end;shown=shown+1 end end end
 bar(m.download_select_mode==1 and selectionHint(m)or"↑↓选择  ○打开  △回播放","L/R 搜索分类")
end
local function downloads(t,m)
 header("下载",nil,m)
 text(18,55,120,"歌曲",1,m.download_tab==0 and C.accent or C.dim);text(110,55,120,"MV",1,m.download_tab==1 and C.accent or C.dim)
 text(200,59,263,tostring(#(m.downloads or{})).." 项  待解析 "..tostring(m.pending_downloads or 0),0,C.dim)
 if #(m.downloads or{})==0 then icon("download",211,112,42);text(45,185,390,"暂无任务 · 歌曲列表△加入后台下载",0,C.dim)
 else local first=math.max(1,(m.download_selected or 1)-2);local shown=0;for i,r in ipairs(m.downloads or{})do if i>=first and shown<3 then local y=86+shown*45;shown=shown+1;if i==m.download_selected then ui.rect(15,y,3,34,C.accent)end
  text(23,y,373,r.name,1);text(415,y,46,tostring(r.percent).."%",0,C.accent);local state=m.download_tab==0 and(r.state==4 and"已暂停 · "or r.state==1 and"下载中 · "or"等待 · ")or(r.state==3 and ui.button_hint("未完成 · ○继续 · ")or r.state==2 and"已完成 · "or"下载中 · ");text(23,y+21,373,state..sizeText(r.bytes).." / "..sizeText(r.total_bytes),0,C.dim);ui.rect(23,y+36,435,2,C.line);ui.rect(23,y+36,435*r.percent/100,2,C.accent)
 end end end
 bar(m.download_tab==0 and"○暂停/继续  SELECT播放  △移除"or"○播放/继续  □导出  △删除","L/R分类  ×返回")
end
local function cache(t,m)
 header("离线音乐",nil,m)
 text(18,55,110,"缓存",1,m.cache_tab==0 and C.accent or C.dim);text(110,55,110,"本地",1,m.cache_tab==1 and C.accent or C.dim)
 text(199,59,264,m.cache_tab==1 and(tostring(m.local_count or 0).." 首 · MP3子目录")or(tostring(m.cache_count or 0).." 首 · "..sizeText(m.cache_bytes)),0,C.dim)
 local selected=m.cache_tab==1 and(m.local_selected or 1)or(m.cache_selected or 1);local shown=0
 for _,r in ipairs(m.cache_tab==1 and(m.local_files or{})or(m.cache or{}))do if r.index>=math.max(1,selected-1)and shown<3 then
  local y=87+shown*44;row(y,r.name,r.artist,r.index==selected);if m.cache_tab==0 then text(355,y+21,105,sizeText(r.bytes),0,C.dim)end;shown=shown+1
 end end
 text(19,224,440,"记忆棒可用 "..tostring(m.free_storage_mb or 0).." MB",0,C.dim)
 bar("↑↓选择  ○播放  △操作","L/R 分类  ×返回")
end
local function lyrics(t,m)
 header("歌词",m.song,m);ui.clip(16,74,464,236);drawLyrics(20,440,m,122,74,236);ui.unclip()
 bar(m.lyric_translation==1 and"□关闭译文  ○暂停  L/R切歌"or"□打开译文  ○暂停  L/R切歌","×返回  ↑↓音量")
end
local categoryNames={"推荐","华语","流行","摇滚","电子","民谣","说唱","轻音乐","古典"}
local categoryScroll=0
local prevGridStart,gridShift,gridPage,gridCategory=nil,0,nil,nil
local function grid(t,m)
 header(m.page==13 and"发现音乐"or"歌单",nil,m)
 local cats=categoryNames;local selected=m.grid_selected or 1;local first=math.floor((selected-1)/6)*6+1
 local context=m.page or lastPage;local cat=m.category_selected
 if gridPage~=context or gridCategory~=cat then prevGridStart=first;gridShift=0;borders.grid=nil
 elseif prevGridStart and first~=prevGridStart then gridShift=first>prevGridStart and 72 or -72;borders.grid=nil end
 prevGridStart=first;gridPage=context;gridCategory=cat
 gridShift=gridShift*(1-ease(m));if math.abs(gridShift)<.1 then gridShift=0 end
 if m.page==13 then
  local selectedCat=(m.category_selected or-1)+2;local target=math.max(0,math.min(9*100-444,(selectedCat-2)*100));categoryScroll=categoryScroll+(target-categoryScroll)*.22
  ui.clip(16,50,464,74);for i,label in ipairs(cats)do local x=18+(i-1)*100-categoryScroll;text(x+34,54,64,label,0,i==selectedCat and C.accent or C.dim)end;ui.unclip()
 end
 ui.clip(12,77,464,230)
 for i=0,5 do local c=(m.cards or{})[first+i];if c then local x=16+(i%3)*150;local y=77+(i//3)*76+gridShift;local chosen=c.index==selected
  local h=68;local drawY=y

  if h>1 then ui.rect(x,drawY,140,h,C.accent)end
  local cover=h>43 and ui.card_cover(c.index,x,drawY,140,h)
  if h>43 then
   if cover then ui.overlay(x,drawY+h-26,140,26,"#000000",185);text(x+8,drawY+h-20,124,c.name,0,"#FFFFFF")
   else inTile=true;icon(c.kind==2 and"library"or"music",x+10,drawY+8,24);text(x+45,drawY+10,86,c.tracks and tostring(c.tracks).." 首"or"歌单",0,"#FFFFFF");text(x+10,drawY+42,120,c.name,0,"#FFFFFF");inTile=false end
  end
  if chosen then focusBorder("grid",x,y,140,68,m)end
 end end
 ui.unclip()
 text(18,222,440,tostring(selected).." / "..tostring(#(m.cards or{})).."  "..(m.page==13 and categoryNames[(m.category_selected or-1)+2]or""),0,C.dim)
 bar("方向选择  按住←→快翻  ○打开","L/R分类  ×返回")
end
local function account(t,m)
 header("账号",m.logged_in==1 and"网易云音乐"or"扫码登录",m)
 if m.logged_in==1 then text(21,89,430,m.login_name or"已登录",2);text(23,139,420,"音乐收藏与歌单已连接",0,C.dim);bar("△刷新  ○账号操作","× 返回")
 else text(22,85,210,"手机扫码登录",1);text(22,124,210,"打开网易云音乐扫一扫",0,C.dim);text(22,148,210,m.qr_state==802 and"已扫码，请在手机确认"or ui.button_hint("○ 获取或刷新二维码"),0,C.dim);if not ui.qr(277,69,160)then ui.rect(277,69,160,160,C.soft);text(289,131,136,"等待二维码",0,C.dim)end;bar("○生成 / 刷新二维码","× 返回")end
end
local pages={home=home,mine=mine,settings=settings,player=player,songs=songs,search=search,downloads=downloads,cache=cache,lyrics=lyrics,grid=grid,square=grid,account=account}
local skin={}
local function beginTurn(action,arg,page,t)
 if ui.begin_navigation then ui.begin_navigation()end
 app.dispatch(action,arg or"")
end
function skin.draw(page,t,dt,m,k)
 refreshPalette()
 local draw=pages[page];if not draw then return false end
 ui.rect(0,0,480,272,C.bg)
 if page~=lastPage then if page=="player"then playerMode=0;previousPlayerMode=0;modeChangedAt=t-.3 end;lastPage=page;entered=t;repeatKey=0 end
 draw(t,m)
 return true -- all page exits/entries are owned by the unified native navigator
end
function skin.input(page,t,dt,m,k)
 if(k.held&k.select)~=0 and(k.pressed&k.l)~=0 then return false end
 refreshPalette()
 if page=="player"and playerMode~=0 and m.menu_open~=1 and(k.pressed&k.square)~=0 then app.dispatch("toggle_translation");return true end
 if page=="player"and m.menu_open~=1 and(k.pressed&k.triangle)~=0 then previousPlayerMode=playerMode;playerMode=(playerMode+1)%3;modeChangedAt=t;sound("confirm");return true end
 if page=="home"then
  if k.pressed&k.back~=0 then if homeSection>1 then homeSection=homeSection-1;tiles=tileSets[homeSection];focus=1 else app.dispatch("mine")end;sound("back");return true end
  if k.pressed&k.l~=0 then homeSection=math.max(1,homeSection-1);tiles=tileSets[homeSection];focus=1;return true end
  if k.pressed&k.r~=0 then homeSection=math.min(3,homeSection+1);tiles=tileSets[homeSection];focus=1;return true end
  if k.pressed&k.select~=0 then app.dispatch("settings");return true end
  local d=repeatDirection(t,k);if d&k.left~=0 then neighbor("left")elseif d&k.right~=0 then neighbor("right")elseif d&k.up~=0 then neighbor("up")elseif d&k.down~=0 then neighbor("down")end
  if k.pressed&k.confirm~=0 then local tile=tiles[focus];if tile.section then homeSection=tile.section;tiles=tileSets[homeSection];focus=1 else beginTurn(tile.action,nil,page,t) end;sound("confirm")end;return true
 end
 if page=="mine"then
  local slot=1;for i,index in ipairs(mineOrder)do if index==m.mine_selected then slot=i end end
  local d=repeatDirection(t,k);if d&k.left~=0 then app.dispatch("mine_focus",tostring(mineOrder[math.max(1,slot-4)]));return true elseif d&k.right~=0 then app.dispatch("mine_focus",tostring(mineOrder[math.min(#mineOrder,slot+4)]));return true elseif d&k.up~=0 then app.dispatch("mine_focus",tostring(mineOrder[math.max(1,slot-1)]));sound("move");return true elseif d&k.down~=0 then app.dispatch("mine_focus",tostring(mineOrder[math.min(#mineOrder,slot+1)]));sound("move");return true end
  if k.pressed&k.confirm~=0 then app.dispatch("mine_select",tostring(m.mine_selected or 8));sound("confirm");return true end
 end
 if page=="grid"or page=="square"then
  local selected=m.grid_selected or 1;local d=repeatDirection(t,k);local count=#(m.cards or{})
  local nextIndex=selected
  local stride=(k.pressed&(k.left|k.right))~=0 and 1 or 6
  if d&k.left~=0 then nextIndex=math.max(1,selected-stride)elseif d&k.right~=0 then nextIndex=math.min(count,selected+stride)elseif d&k.up~=0 then nextIndex=math.max(1,selected-3)elseif d&k.down~=0 then nextIndex=math.min(count,selected+3)end
  if nextIndex~=selected then app.dispatch("grid_select",tostring(nextIndex));sound("move");return true end
  if page=="grid"and(k.pressed&(k.l|k.r))~=0 then local step=(k.pressed&k.l)~=0 and-6 or 6;app.dispatch("grid_select",tostring(math.max(1,math.min(count,selected+step))));return true end
  if page=="square"and(k.pressed&(k.l|k.r))~=0 then local cat=(m.category_selected or-1)+((k.pressed&k.r~=0)and 1 or-1);app.dispatch("category_select",tostring(math.max(-1,math.min(7,cat))));return true end
  if k.pressed&k.confirm~=0 then local c=(m.cards or{})[selected];if c then beginTurn(c.kind==2 and"open_artist"or"open_playlist",c.id,page,t);sound("confirm")end;return true end
  if k.pressed&k.back~=0 then app.dispatch("home");sound("back");return true end
  return true
 end

 if pages[page]then if k.pressed&k.back~=0 then sound("back")elseif k.pressed&k.confirm~=0 then sound("confirm")elseif k.pressed&(k.up|k.down|k.left|k.right)~=0 then sound("move")end end
 return false
end
return skin
