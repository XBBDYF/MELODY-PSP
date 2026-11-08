-- XMB / 480x272. Original geometry; native business state and modal ownership.
-- Program 3.5.0 baseline. Newer capabilities are optional.
local C={bg="#122C54",fg="#FFFFFF",dim="#B6C8DC",accent="#FFFFFF",soft="#18365B",line="#7289A4"}
local category,subIndex,visualCategory=3,1,3
local lastPage,repeatKey,repeatAt="",0,0
local playerMode,previousPlayerMode,modeChangedAt=0,0,-1
local function preference(id,fallback)if app.option then return app.option(id,fallback)end return fallback end
local function sound(id)if ui.sound and id=="confirm"then ui.sound("confirm")end end
local function avatar(...)if ui.avatar then return ui.avatar(...)end return false end
local function text(x,y,w,s,font,col)ui.label(x,y,w,tostring(s or""),font or 0,col or C.fg)end
local function line(x,y,x2,y2,col,width)ui.line(x,y,x2,y2,col or C.fg,width or 1)end
local categories={
 {name="音乐",icon="music",rows={{"每日推荐","daily"},{"推荐歌单","folder","discovery"},{"正在播放","player"},{"最近播放","recent"}}},
 {name="曲库",icon="photo",rows={{"我喜欢的音乐","favorites"},{"我的歌单","my_playlists"},{"离线音乐","folder","offline"},{"账号管理","account"}}},
 {name="搜索",icon="search",rows={{"搜索歌曲","open","search:0"},{"搜索歌单","open","search:1"},{"搜索专辑","open","search:2"},{"搜索歌手","open","search:3"},{"搜索声音","open","search:4"}}},
 {name="下载",icon="download",rows={{"歌曲任务","open","downloads:0"},{"MV文件","open","downloads:1"}}},
 {name="设置",icon="settings",rows={{"播放器与系统","settings"},{"主题与配色","skins"},{"账号管理","account"},{"音乐来源","sources"},{"关于MELODY","about"}}}}
category,visualCategory=1,1
local folders={
 discovery={title="推荐歌单",rows={{"首页推荐","open","discovery:-1"},{"华语","open","discovery:0"},{"流行","open","discovery:1"},{"摇滚","open","discovery:2"},{"电子","open","discovery:3"},{"民谣","open","discovery:4"},{"说唱","open","discovery:5"},{"轻音乐","open","discovery:6"},{"古典","open","discovery:7"}}},
 offline={title="离线音乐",rows={{"已缓存歌曲","open","cache:0"},{"本地文件","open","cache:1"}}}}
local folderStack,folderSelections={},{}
local currentFolder=nil
local selectionMotion={}
local function visualSelection(key,target,m)
 local value=selectionMotion[key]or target
 local a=1-math.exp(-math.max(0,math.min(.06,m.frame_dt or .016))/.10)
 value=value+(target-value)*a;selectionMotion[key]=value;return value
end
local transferFolder,offlineFolder=true,true
local transferSelection,offlineSelection=1,1
local contentIntent={}
-- Each silhouette is deliberately drawn locally, with no platform glyph/font dependency.
local function icon(kind,x,y,s,col)
 col=col or C.fg
 if kind=="music"then
  line(x+s*.35,y+s*.18,x+s*.35,y+s*.78,col,3);line(x+s*.35,y+s*.18,x+s*.83,y+s*.07,col,3);line(x+s*.83,y+s*.07,x+s*.83,y+s*.67,col,3)
  ui.circle(x+s*.22,y+s*.79,s*.13,col);ui.circle(x+s*.7,y+s*.68,s*.13,col)
 elseif kind=="settings"then
  ui.rect(x+s*.12,y+s*.34,s*.76,s*.49,col);line(x+s*.35,y+s*.32,x+s*.35,y+s*.16,col,2);line(x+s*.35,y+s*.16,x+s*.65,y+s*.16,col,2);line(x+s*.65,y+s*.16,x+s*.65,y+s*.32,col,2);line(x+s*.12,y+s*.55,x+s*.88,y+s*.55,C.bg,1)
 elseif kind=="photo"then
  line(x+s*.08,y+s*.17,x+s*.92,y+s*.17,col,2);line(x+s*.08,y+s*.17,x+s*.08,y+s*.8,col,2);line(x+s*.92,y+s*.17,x+s*.92,y+s*.8,col,2);line(x+s*.08,y+s*.8,x+s*.92,y+s*.8,col,2)
  line(x+s*.15,y+s*.7,x+s*.4,y+s*.43,col,2);line(x+s*.4,y+s*.43,x+s*.66,y+s*.72,col,2);ui.circle(x+s*.73,y+s*.36,s*.08,col)
 elseif kind=="video"then
  ui.rect(x+s*.06,y+s*.24,s*.88,s*.55,col);for i=0,3 do ui.rect(x+s*(.13+i*.21),y+s*.28,s*.11,s*.09,C.bg);ui.rect(x+s*(.13+i*.21),y+s*.65,s*.11,s*.09,C.bg)end
 elseif kind=="game"then
  line(x+s*.23,y+s*.3,x+s*.77,y+s*.3,col,3);line(x+s*.23,y+s*.3,x+s*.08,y+s*.76,col,4);line(x+s*.77,y+s*.3,x+s*.92,y+s*.76,col,4);line(x+s*.08,y+s*.76,x+s*.39,y+s*.61,col,3);line(x+s*.61,y+s*.61,x+s*.92,y+s*.76,col,3);line(x+s*.39,y+s*.61,x+s*.61,y+s*.61,col,3);line(x+s*.23,y+s*.4,x+s*.23,y+s*.59,col,2);line(x+s*.14,y+s*.49,x+s*.32,y+s*.49,col,2);ui.circle(x+s*.73,y+s*.46,s*.045,col)
 elseif kind=="network"or kind=="psn"then
  ui.circle(x+s*.5,y+s*.48,s*.38,col);ui.circle(x+s*.5,y+s*.48,math.max(1,s*.38-1.5),C.bg);line(x+s*.12,y+s*.48,x+s*.88,y+s*.48,col,1);line(x+s*.5,y+s*.1,x+s*.5,y+s*.86,col,1);line(x+s*.22,y+s*.27,x+s*.78,y+s*.27,col,1);line(x+s*.22,y+s*.68,x+s*.78,y+s*.68,col,1)
  if kind=="psn"then line(x+s*.3,y+s*.92,x+s*.7,y+s*.92,col,2)end
 elseif kind=="search"then ui.circle(x+s*.4,y+s*.36,s*.28,col);line(x+s*.6,y+s*.59,x+s*.91,y+s*.9,col,3)
 elseif kind=="download"then line(x+s*.5,y+s*.05,x+s*.5,y+s*.65,col,2);line(x+s*.25,y+s*.4,x+s*.5,y+s*.65,col,2);line(x+s*.5,y+s*.65,x+s*.75,y+s*.4,col,2);line(x+s*.16,y+s*.8,x+s*.84,y+s*.8,col,2)
  elseif kind=="pause"then ui.rect(x+s*.19,y,s*.18,s,col);ui.rect(x+s*.61,y,s*.18,s,col)
 elseif kind=="prev"or kind=="next"then local d=kind=="next"and 1 or-1;local cx=x+s*.5;line(cx-d*s*.35,y,cx+d*s*.25,y+s*.5,col,2);line(cx+d*s*.25,y+s*.5,cx-d*s*.35,y+s,col,2);line(cx+d*s*.4,y,cx+d*s*.4,y+s,col,2)
 else line(x+s*.2,y+s*.1,x+s*.8,y+s*.5,col,2);line(x+s*.8,y+s*.5,x+s*.2,y+s*.9,col,2)end
end
local colorPalettes={
 {bg="#071A39",soft="#18365B",line="#647FA4",dim="#AFBED6",accent="#D8EFFF",wave="#76B5E5",asset="blue"},
 {bg="#21133F",soft="#382451",line="#806C9E",dim="#C8B9DB",accent="#E9D8FF",wave="#C7A1F3",asset="purple"},
 {bg="#092E2A",soft="#1B4740",line="#527F73",dim="#AECDC1",accent="#CAFFE3",wave="#85DFC1",asset="green"},
 {bg="#42210E",soft="#603A22",line="#9B7756",dim="#DEC4A5",accent="#FFE8BD",wave="#F5C47C",asset="amber"},
 {bg="#3D172C",soft="#5B304A",line="#986080",dim="#DAB5CD",accent="#FFE0EE",wave="#E8A2D1",asset="rose"},
 {bg="#152330",soft="#304453",line="#6F899D",dim="#BFCFD9",accent="#FFFFFF",wave="#CBDCEA",asset="silver"}}
local function updateColors()
 local index=math.max(0,math.min(5,preference("color_style",0)))+1;local p=colorPalettes[index]
 C.bg=p.bg;C.soft=p.soft;C.line=p.line;C.dim=p.dim;C.accent=p.accent;C.fg="#FFFFFF";return p
end
local function background(t)
 local p=updateColors();ui.rect(0,0,480,272,C.bg)
 if ui.image then ui.image(p.asset,0,0,480,272)end
 if ui.wave then ui.wave(preference("wave_motion",1)==1 and t or 0,p.wave)end
end
local function status(m)
 local battery=math.max(0,math.min(100,m.battery or 0));text(315,13,110,m.clock or"",0,C.fg)
 line(441,15,465,15);line(441,15,441,25);line(441,25,465,25);line(465,15,465,25);ui.rect(466,18,2,5,C.fg);ui.rect(444,18,18*battery/100,5,C.fg)
 if m.show_speed==1 then text(16,13,150,tostring(m.network_kbps or 0).." KB/s",0,C.dim)end
 if m.show_memory==1 then text(166,13,145,string.format("%.1f MB",(m.memory_used_kb or 0)/1024),0,C.dim)end
end
local function header(title,sub,m)
 status(m);local glyph=categories[category].icon;if string.find(title,"搜索",1,true)then glyph="search"elseif string.find(title,"下载",1,true)then glyph="download"elseif string.find(title,"曲库",1,true)then glyph="photo"elseif string.find(title,"设置",1,true)then glyph="settings"end;icon(glyph,32,44,27);text(77,44,385,title,1)
 if sub then text(78,65,380,sub,0,C.dim)end
end
local hints={home="←→ 分类   ↑↓ 项目   L/R 快切   ○ 进入",player="SELECT 菜单   □ 喜欢   ↑↓ 音量   × 列表",settings="↑↓ 选择   ←→ 修改   ○ 进入   × 返回",search="L/R 分类   SELECT 多选   × 返回",downloads="L/R 歌曲 / MV   ○ 操作   × 返回",cache="L/R 缓存 / 本地   ○ 播放   × 返回"}
local selectionFooter=nil
local function bar(left,right)
 local compact={home="←→分类  ↑↓选择  ○进入  ×返回",player="○暂停/继续  L/R切歌  △歌词  SELECT菜单  ×返回",lyrics="○暂停  □译文  L/R切歌  ×返回",songs="○播放  L多选  □勾选  △下载  ×返回",search="○选择/打开  △搜索/编辑  L/R分类  ×返回",downloads="○暂停/继续  SELECT播放  △移除  ×返回",cache="○播放  ←→快移  ×返回",grid="↑↓选择  ○打开  ←→快移  ×返回",square="↑↓选择  ○打开  ←→快移  ×分类",settings="↑↓选择  ←→调整  ○确认  ×返回",mine="↑↓选择  ○打开  △播放  ×返回",account="○账号操作  ×返回"}
 local value=selectionFooter or compact[lastPage]or left or"○进入  ×返回"
 if(lastPage=="downloads"and transferFolder)or(lastPage=="cache"and offlineFolder)or(lastPage=="home"and currentFolder)then value="↑↓选择  ○打开文件夹  ←→快移  ×上一级"end
 text(18,254,446,value,0,C.fg)
end
local function row(y,label,value,selected)
 if selected then text(46,y+2,22,">",1)end
 text(77,y,382,label,selected and 1 or 0,selected and C.fg or C.dim)
 if value and value~=""then text(78,y+20,380,value,0,C.dim)end
end
local function songRow(y,s,selected,m)
 if m.download_select_mode~=1 then local tag=(s.fee or 0)<0 and "酷我" or (s.playable==2 and "三方" or nil);if selected then text(25,y,20,">",0,C.accent)end;text(50,y,tag and 360 or 400,s.name,0,selected and C.accent or C.fg);if tag then text(420,y,40,tag,0,C.accent)end;text(50,y+17,400,s.artist,0,C.dim);return end
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
 if t>=repeatAt then repeatAt=t+.12;return down end;return 0
end
local function folderIcon(x,y,size,col)
 ui.rect(x,y+size*.22,size,size*.65,col);ui.rect(x+size*.08,y+size*.08,size*.42,size*.25,col)
end
local function directory(title,rows,selected,m)
 status(m);text(28,45,420,title,1,C.fg)
 local visual=visualSelection("directory:"..title,selected,m)
 ui.clip(24,80,468,240)
 for i=math.max(1,math.floor(visual)-1),math.min(#rows,math.ceil(visual)+3)do local y=121+(i-visual)*35;local chosen=i==selected
  local size=19+4*math.max(0,1-math.abs(i-visual));folderIcon(51,y+1,size,chosen and C.accent or C.dim)
  text(88,y+2,360,rows[i][1],chosen and 1 or 0,chosen and C.fg or C.dim)
 end
 ui.unclip()
 bar("↑↓选择  ○打开文件夹  ←→快移","×上一级  △正在播放")
end
local function home(t,m)
 if currentFolder then local node=folders[currentFolder];directory("音乐 / "..node.title,node.rows,subIndex,m);return end
 status(m);local dt=math.max(0,math.min(.06,m.frame_dt or .016));visualCategory=visualCategory+(category-visualCategory)*math.min(1,dt*16)
 for i,c in ipairs(categories)do local x=108+(i-visualCategory)*80;local chosen=i==category;local size=preference("icon_zoom",1)==1 and(24+8*math.max(0,1-math.abs(i-visualCategory)))or 26
  if x>=-35 and x<=480 then icon(c.icon,x-size/2,70-size/2,size,chosen and C.fg or C.dim);if chosen then text(x-28,94,112,c.name,0,C.fg)end end
 end
 local rows=categories[category].rows;local visual=visualSelection("home:"..category,subIndex,m)
 ui.clip(70,122,472,240)
 for i=math.max(1,math.floor(visual)-1),math.min(#rows,math.ceil(visual)+3)do local y=132+(i-visual)*34;local chosen=i==subIndex
  local size=19+4*math.max(0,1-math.abs(i-visual));folderIcon(96,y-1,size,chosen and C.accent or C.dim);text(140,y,326,rows[i][1],chosen and 1 or 0,chosen and C.fg or C.dim)
 end
 ui.unclip()
 bar("←→分类  ↑↓项目  ○进入","×上一级  △正在播放  SELECT设置")
end
local function mine(t,m)
 header("我的音乐",m.logged_in==1 and(m.login_name or"")or"MELODY 功能入口",m)
 if ui.avatar then avatar(29,91,32,32)end
 local selected=m.mine_selected or 1;local first=math.max(1,selected-1);local shown=0
 for _,r in ipairs(m.mine or{})do if r.index>=first and shown<4 then row(88+shown*34,r.label,nil,r.index==selected);shown=shown+1 end end
 bar("↑↓ 选择   ○ 进入   △ 歌曲详情","L/R 主菜单   × 返回   SELECT+L 皮肤中心")
end

local function sizeText(bytes)
 bytes=math.max(0,bytes or 0);if bytes>=1048576 then return string.format("%.1f MB",bytes/1048576)elseif bytes>=1024 then return string.format("%.0f KB",bytes/1024)else return tostring(bytes).." B"end
end

local function settings(t,m)
 header("设置","系统  /  个性化",m);local selected=m.setting_selected or 1;local first=math.max(1,selected-2);local shown=0
 for _,r in ipairs(m.settings or{})do if r.index>=first and shown<4 then row(78+shown*39,r.label,r.value,r.index==selected);shown=shown+1 end end
 text(425,224,40,tostring(selected).."/"..tostring((m.settings or{}).total or 24),0,C.dim);bar("↑↓选择  ←→修改  ○进入","× 我的")
end
-- XMB rendering fragment, concatenated by the parent template (not a module).
-- Requires shared C, ui, text, icon, background, status, bar, header.
-- Parent owns native input, folders, navigation, lyrics, and color_style 0..5.
-- All colors come from C: white foreground on dark themes, dark on silver.
local xmbDiscAngle,xmbDiscTime=0,nil
local function xmbPlayer(t,m)
 if ui.music_fx then ui.music_fx(preference('music_fx',0),t,m.audio_level or 0)end
 status(m);text(24,43,410,"音乐 / 正在播放",0,C.dim)
 local song=m.song or"";local paused=m.paused==1
 local dt=xmbDiscTime and math.max(0,math.min(.1,t-xmbDiscTime))or 0;xmbDiscTime=t
 if not paused and song~=""then xmbDiscAngle=(xmbDiscAngle+dt*18)%360 end
 if m.disc_player==1 and ui.disc then ui.disc(24,92,80,xmbDiscAngle)
 elseif not ui.cover(24,92,80,80)then
  ui.rect(24,92,80,80,C.soft);icon("music",46,114,36,C.fg)
 end
 text(122,97,330,song~=""and song or"选择一首音乐",1,C.fg)
 text(122,124,330,m.artist or"",0,C.fg)
 text(122,148,330,(m.quality or"").."   音量 "..tostring(m.volume or 0).."%",0,C.dim)
 local state=m.buffering==1 and"缓冲中"or paused and"已暂停"or song~=""and"播放中"or"等待播放"
 if m.cache_percent and m.cache_percent>=0 and m.cache_percent<100 then state=state.." · 缓存 "..tostring(m.cache_percent).."%"end
 text(24,203,350,state,0,C.dim)
 local duration=math.max(1,m.duration_ms or 0);local position=math.max(0,m.position_ms or 0)
 ui.rect(24,190,432,2,C.line)
 ui.rect(40,190,432*math.min(1,position/duration),2,C.accent)
 local function stamp(ms)return string.format("%d:%02d",math.floor(ms/60000),math.floor(ms/1000)%60)end
 text(332,203,132,stamp(position).." / "..stamp(m.duration_ms or 0),0,C.dim)
 icon(paused and"play"or"pause",24,220,12,C.fg);text(45,221,270,paused and"暂停"or"播放",0,C.dim)
 bar("○ 暂停/继续   L/R 切歌   ←→ 进度","△ 歌词   SELECT 菜单   □ 喜欢   × 返回")
end

local function xmbSearch(t,m)
 header("搜索",nil,m)
 icon("search",24,67,15,C.dim);local pre=(m.ime_active or 0)==1 and(m.ime_pre or"")or"";text(50,65,330,(m.query and m.query~="")and m.query or"歌曲、歌手、歌单",1,C.fg);if pre~=""then text(50+math.min(ui.text_width(m.query or"",1),300),65,300,pre,0,C.accent)end;if m.search_edit==1 then text(400,65,56,(m.ime_mode or 1)==2 and"假名"or((m.ime_mode or 1)==3 and"英文"or"拼音"),0,C.accent)end
 ui.rect(24,90,432,1,C.line)
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
   for i=0,3 do local idx=off+i+1;local v=cands[idx];if v and v~=""then local x=56+i*68;local sel=m.chip_row==2 and (m.ime_selected or 1)==idx;if sel then ui.rect(x,119,64,20,C.accent)end;text(x+3,122,58,v,0,sel and C.bg or C.fg)end end
   if off+4<#cands then text(330,122,8,">",0,C.accent)end
  end
  text(356,101,110,"历史",0,C.dim)
  for i=1,math.min(#hist,2)do local y=115+(i-1)*16;local sel=m.chip_row==3 and (m.hist_selected or 1)==i;if sel then ui.rect(353,y-1,116,15,C.accent)end;text(357,y,110,hist[i],0,sel and C.bg or C.dim)end
  local letters={"qwertyuiop","asdfghjkl","zxcvbnm"}
  for r=0,2 do for col=0,(r<2 and 9 or 8)do
   local label=col<#letters[r+1]and string.sub(letters[r+1],col+1,col+1)or r==1 and"←"or col==7 and"空格"or"搜索"
   local x=17+col*45;local w=41;if r==2 and col==7 then w=76 elseif r==2 and col==8 then x=413;w=48 end
   local y=145+r*29
   if m.kb_row==r and m.kb_col==col and m.suggest_focused~=1 then ui.rect(x,y,w,25,C.soft)end
   text(x+9,y+6,w-11,label,0,C.fg)
  end end
  bar("输入拼音   ○ 选择   △ 搜索","L/R 搜索分类   × 返回");return
 end
 local kinds={"歌曲","歌单","专辑","歌手","声音"}
 text(24,99,430,"搜索 / "..(kinds[(m.search_kind or 0)+1]or"歌曲"),0,C.dim)
 if m.search_state~=1 then
  local selected=m.selected or 1;local first=math.max(1,selected-1);local shown=0
  for i,s in ipairs((m.search_kind or 0)==0 and(m.songs or{})or(m.cards or{}))do local index=s.index or i
   if index>=first and shown<3 then local y=120+shown*34;local col=index==selected and C.accent or C.fg
    if index==selected then text(24,y,18,">",0,C.accent)end
    if(m.search_kind or 0)==0 then songRow(y,s,index==selected,m)else text(48,y,400,s.name,0,col);text(48,y+17,400,s.subtitle,0,C.dim)end;shown=shown+1
   end
  end
  if shown==0 then text(48,132,390,"暂无结果 · △ 编辑搜索",0,C.dim)end
 end
 bar(m.download_select_mode==1 and"□勾选  △下载选中  SELECT退出"or(m.song and m.song~=""and"○打开  □下载  △正在播放"or"○打开  □下载  △编辑"),"L/R 搜索分类   SELECT 多选   × 返回")
end

local function xmbRows(t,m)
 local songs=m.songs or{};local selected=m.selected or 1
 header((m.list_title and m.list_title~="")and m.list_title or"音乐", "音乐 / 歌单 · "..tostring(songs.total or #songs).." 首",m)
 local first=math.max(1,selected-2);local shown=0
 for i,s in ipairs(songs)do local index=s.index or i;if index>=first and shown<4 then local y=90+shown*36
  if index==selected then text(25,y,20,">",0,C.accent)end
  songRow(y,s,index==selected,m);shown=shown+1
 end end
 if shown==0 then text(50,126,400,"暂无歌曲",0,C.dim)end
 bar(m.download_select_mode==1 and"□ 勾选   △ 下载选中   L 退出多选"or"○ 播放   □ 下载   △ 歌曲详情   L 多选","←→ 快移   × 返回")
end

local function xmbTransfers(t,m)
 local mv=m.download_tab==1;local rows=m.downloads or{};local selected=m.download_selected or 1
 header(mv and"下载 / MV"or"下载 / 歌曲",tostring(#rows).." 项 · 待解析 "..tostring(m.pending_downloads or 0),m)
 local states={[0]="等待中",[1]="下载中",[2]="已完成",[3]=mv and"未完成 · ○继续"or"失败",[4]="已暂停"}
 local first=math.max(1,selected-1);local shown=0
 for i,r in ipairs(rows)do if i>=first and shown<3 then local y=90+shown*44;local pct=math.max(0,math.min(100,r.percent or 0))
  if i==selected then text(24,y,18,">",0,C.accent)end
  text(48,y,340,r.name,0,i==selected and C.accent or C.fg)
  text(48,y+18,330,(states[r.state]or"等待中").." · "..tostring(pct).."%",0,C.dim)
  ui.rect(48,y+35,392,1,C.line);ui.rect(48,y+35,392*pct/100,1,C.accent);shown=shown+1
 end end
 if shown==0 then text(48,126,392,mv and"暂无 MV 文件或任务"or"暂无任务 · 在歌曲列表加入下载",0,C.dim)end
 bar(mv and"○ 播放/取消   □ 导出   △ 删除"or"○ 暂停/继续   SELECT 播放   □ 清空   △ 移除","× 返回下载文件夹")
end

local function xmbOffline(t,m)
 local isLocal=m.cache_tab==1;local rows=isLocal and(m.local_files or{})or(m.cache or{})
 local selected=isLocal and(m.local_selected or 1)or(m.cache_selected or 1)
 local count=isLocal and(m.local_count or #rows)or(m.cache_count or #rows)
 header(isLocal and"离线音乐 / 本地"or"离线音乐 / 缓存",tostring(count).." 首 · 记忆棒可用 "..tostring(m.free_storage_mb or 0).." MB",m)
 local first=math.max(1,selected-1);local shown=0
 for i,r in ipairs(rows)do local index=r.index or i;if index>=first and shown<4 then local y=90+shown*36
  if index==selected then text(24,y,18,">",0,C.accent)end
  text(48,y,392,r.name,0,index==selected and C.accent or C.fg)
  text(48,y+17,392,r.artist or(isLocal and"本地文件"or"缓存音乐"),0,C.dim);shown=shown+1
 end end
 if shown==0 then text(48,126,392,isLocal and"本地文件夹暂无音乐"or"暂无缓存音乐",0,C.dim)end
 bar("↑↓ 选择   ○ 播放   △ 操作","× 返回离线文件夹")
end

local player,songs,search,downloads,cache=xmbPlayer,xmbRows,xmbSearch,xmbTransfers,xmbOffline
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
local function lyrics(t,m)
 header("歌词",m.song,m);ui.clip(16,74,464,236);drawLyrics(20,440,m,122,74,236);ui.unclip()
 bar(m.lyric_translation==1 and"□关闭译文  ○暂停  L/R切歌"or"□打开译文  ○暂停  L/R切歌","×返回  ↑↓音量")
end
local function account(t,m)
 header("账号",m.logged_in==1 and"网易云音乐"or"扫码登录",m)
 if m.logged_in==1 then text(21,89,430,m.login_name or"已登录",2);text(23,139,420,"音乐收藏与歌单已连接",0,C.dim);bar("△刷新  ○账号操作","× 返回")
 else text(22,85,210,"手机扫码登录",1);text(22,124,210,"打开网易云音乐扫一扫",0,C.dim);text(22,148,210,m.qr_state==802 and"已扫码，请在手机确认"or"○ 获取或刷新二维码",0,C.dim);if not ui.qr(277,69,160)then ui.rect(277,69,160,160,C.soft);text(289,131,136,"等待二维码",0,C.dim)end;bar("○生成 / 刷新二维码","× 返回")end
end

local categoryNames={"推荐","华语","流行","摇滚","电子","民谣","说唱","轻音乐","古典"}
local function grid(t,m)
 local selected=m.grid_selected or 1
 local sub=lastPage=="square"and categoryNames[(m.category_selected or-1)+2]or"云端歌单"
 header(lastPage=="square"and"音乐 / 推荐歌单"or"曲库 / 歌单",sub,m)
 local visual=visualSelection("grid:"..lastPage..":"..tostring(m.category_selected),selected,m)
 if #(m.cards or{})==0 then text(77,124,370,"这个文件夹暂时没有歌单",0,C.dim)end
 ui.clip(24,84,468,240)
 for i=math.max(1,math.floor(visual)-1),math.min(#(m.cards or{}),math.ceil(visual)+3)do local c=m.cards[i];local y=91+(i-visual)*44
  if not ui.card_cover(c.index,78,y,34,34)then icon("music",82,y+3,25,C.dim)end
  if c.index==selected then text(46,y+6,22,">",1)end
  text(123,y+2,338,c.name,c.index==selected and 1 or 0,c.index==selected and C.fg or C.dim)
  text(124,y+23,335,c.subtitle or tostring(c.tracks or 0).." 首",0,C.dim)
 end
 ui.unclip()
 bar("↑↓选择  ○打开  ←→快移","×返回分类文件夹  SELECT+L主题")
end
local function downloadsPage(t,m)
 if transferFolder then directory("下载",{{"歌曲任务"},{"MV文件"}},transferSelection,m)else downloads(t,m)end
end
local function offlinePage(t,m)
 if offlineFolder then directory("曲库 / 离线音乐",{{"缓存歌曲"},{"本地文件"}},offlineSelection,m)else cache(t,m)end
end
local pages={home=home,mine=mine,settings=settings,player=player,songs=songs,search=search,downloads=downloadsPage,cache=offlinePage,lyrics=lyrics,grid=grid,square=grid,account=account}
local skin={}
local nativeNavigation=false
local function navigate(action,arg)
 if not nativeNavigation then
  if action=="xmb_back"then action="home"
  elseif action=="xmb_open"then local value=arg or"";if string.sub(value,1,10)=="downloads:"then action="downloads"elseif string.sub(value,1,6)=="cache:"then action=string.sub(value,7)=="1"and"local"or"cache"elseif string.sub(value,1,7)=="search:"then action="search"elseif string.sub(value,1,10)=="discovery:"then action="square"end;arg="" end
 end
 if ui.begin_navigation then ui.begin_navigation()end
 app.dispatch(action,arg or"")
end
local function openItem(item)
 if item[2]=="folder"then folderStack[#folderStack+1]={folder=currentFolder,selected=subIndex};currentFolder=item[3];subIndex=folderSelections[currentFolder]or 1
 elseif item[2]=="open"then
  if string.sub(item[3],1,10)=="downloads:"then transferFolder=false;contentIntent.downloads=true end
  if string.sub(item[3],1,6)=="cache:"then offlineFolder=false;contentIntent.cache=true end
  navigate("xmb_open",item[3])
 else navigate(item[2])end
 sound("confirm")
end
local function back()
 if currentFolder then folderSelections[currentFolder]=subIndex;local old=table.remove(folderStack);currentFolder=old and old.folder or nil;subIndex=old and old.selected or 1 else navigate("xmb_back")end
end
function skin.draw(page,t,dt,m,k)
 nativeNavigation=m.xmb_navigation==1
 local draw=pages[page];if not draw then return false end
 if page~=lastPage then
  if page=="downloads"then if not contentIntent.downloads then transferFolder=true end;contentIntent.downloads=nil end
  if page=="cache"then if not contentIntent.cache then offlineFolder=true end;contentIntent.cache=nil end
  lastPage=page;repeatKey=0
 end
 selectionFooter=nil;if m.download_select_mode==1 and(page=="songs"or(page=="search"and(m.search_kind or 0)==0))then selectionFooter=selectionHint(m).."  ×返回"end
 background(t);draw(t,m);return true
end
function skin.input(page,t,dt,m,k)
 nativeNavigation=m.xmb_navigation==1
 updateColors()
 if m.menu_open==1 then return false end
 if (k.held&k.select)~=0 and(k.pressed&k.l)~=0 then return false end
 if page=="home"then
  if(k.pressed&k.back)~=0 then back();return true end
  if(k.pressed&k.triangle)~=0 then navigate(m.song and m.song~=""and"player"or"mine");return true end
  if(k.pressed&k.select)~=0 then navigate("settings");return true end
  local d=repeatDirection(t,k)
  if currentFolder then local rows=folders[currentFolder].rows
   if(d&k.up)~=0 then subIndex=math.max(1,subIndex-1)elseif(d&k.down)~=0 then subIndex=math.min(#rows,subIndex+1)
   elseif(d&k.left)~=0 then subIndex=math.max(1,subIndex-4)elseif(d&k.right)~=0 then subIndex=math.min(#rows,subIndex+4)end
   if(k.pressed&k.confirm)~=0 then openItem(rows[subIndex])end
  else
   local step=0;if(d&k.left)~=0 or(k.pressed&k.l)~=0 then step=-1 elseif(d&k.right)~=0 or(k.pressed&k.r)~=0 then step=1 end
   if step~=0 then folderSelections["root"..category]=subIndex;category=math.max(1,math.min(#categories,category+step));subIndex=folderSelections["root"..category]or 1
   elseif(d&k.up)~=0 then subIndex=math.max(1,subIndex-1)elseif(d&k.down)~=0 then subIndex=math.min(#categories[category].rows,subIndex+1)end
   if(k.pressed&k.confirm)~=0 then openItem(categories[category].rows[subIndex])end
  end
  return true
 end
 if page=="downloads"or page=="cache"then
  local isTransfers=page=="downloads";local folder=offlineFolder;if isTransfers then folder=transferFolder end;local selected=isTransfers and transferSelection or offlineSelection
  if(k.pressed&k.back)~=0 then if folder then navigate("xmb_back")elseif isTransfers then transferFolder=true else offlineFolder=true end;return true end
  if folder then
   local d=repeatDirection(t,k);if(d&(k.up|k.left))~=0 then selected=1 elseif(d&(k.down|k.right))~=0 then selected=2 end
   if isTransfers then transferSelection=selected else offlineSelection=selected end
   if(k.pressed&k.confirm)~=0 then if isTransfers then transferFolder=false else offlineFolder=false end;navigate("xmb_open",(isTransfers and"downloads:"or"cache:")..tostring(selected-1))end
   return true
  end
  if(k.pressed&(k.l|k.r))~=0 then if isTransfers then transferSelection=(m.download_tab or 0)+1;transferFolder=true else offlineSelection=(m.cache_tab or 0)+1;offlineFolder=true end;return true end
  return false
 end
 if page=="player"and(k.pressed&k.triangle)~=0 then navigate("lyrics");return true end
 if(k.pressed&k.back)~=0 then navigate("xmb_back");return true end
 if page=="grid"or page=="square"then
  local selected=m.grid_selected or 1;local d=repeatDirection(t,k);local count=#(m.cards or{});local target=selected
  if(d&k.up)~=0 then target=math.max(1,selected-1)elseif(d&k.down)~=0 then target=math.min(count,selected+1)elseif(d&k.left)~=0 then target=math.max(1,selected-4)elseif(d&k.right)~=0 then target=math.min(count,selected+4)end
  if target~=selected and count>0 then app.dispatch("grid_select",tostring(target));return true end
  if(k.pressed&k.confirm)~=0 then local c=(m.cards or{})[selected];if c then navigate(c.kind==2 and"open_artist"or"open_playlist",c.id)end;return true end
  if(k.pressed&(k.l|k.r))~=0 then return true end
  return true
 end
 return false
end
return skin
