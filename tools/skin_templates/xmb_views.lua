-- XMB rendering fragment, concatenated by the parent template (not a module).
-- Requires shared C, ui, text, icon, background, status, bar, header.
-- Parent owns native input, folders, navigation, lyrics, and color_style 0..5.
-- All colors come from C: white foreground on dark themes, dark on silver.
local xmbDiscAngle,xmbDiscTime=0,nil
local function xmbPlayer(t,m)
 if ui.music_fx then ui.music_fx(preference('music_fx',0),t,m.audio_level or 0)end
 header("正在播放",nil,m)
 local song=m.song or"";local paused=m.paused==1
 local dt=xmbDiscTime and math.max(0,math.min(.1,t-xmbDiscTime))or 0;xmbDiscTime=t
 if not paused and song~=""then xmbDiscAngle=(xmbDiscAngle+dt*18)%360 end
 if m.disc_player==1 and ui.disc then ui.disc(40,94,96,xmbDiscAngle)
 elseif not ui.cover(40,94,96,96)then
  ui.rect(40,94,96,96,C.soft);icon("music",68,122,40,C.fg)
 end
 text(158,94,286,song~=""and song or"选择一首音乐",1,C.fg)
 text(158,122,286,m.artist or"",0,C.fg)
 text(158,144,286,(m.quality or"").."   音量 "..tostring(m.volume or 0).."%",0,C.dim)
 local state=m.buffering==1 and"缓冲中"or paused and"已暂停"or song~=""and"播放中"or"等待播放"
 if m.cache_percent and m.cache_percent>=0 and m.cache_percent<100 then state=state.." · 缓存 "..tostring(m.cache_percent).."%"end
 text(158,165,286,state,0,C.dim)
 local duration=math.max(1,m.duration_ms or 0);local position=math.max(0,m.position_ms or 0)
 ui.rect(40,190,400,2,C.line)
 ui.rect(40,190,400*math.min(1,position/duration),2,C.accent)
 local function stamp(ms)return string.format("%d:%02d",math.floor(ms/60000),math.floor(ms/1000)%60)end
 text(40,200,100,stamp(position),0,C.dim);text(395,200,65,stamp(m.duration_ms or 0),0,C.dim)
 icon("prev",188,205,16,C.fg);icon(paused and"play"or"pause",232,202,21,C.fg);icon("next",280,205,16,C.fg)
 bar("○ 暂停/继续   L/R 切歌   ←→ 进度","△ 歌词   SELECT 菜单   □ 喜欢   × 返回")
end

local function xmbSearch(t,m)
 header("搜索",nil,m)
 icon("search",24,67,15,C.dim);text(50,65,402,(m.query and m.query~="")and m.query or"歌曲、歌手、歌单",1,C.fg)
 ui.rect(24,90,432,1,C.line)
 if m.search_edit==1 then
  local chips={"周杰伦","林俊杰","邓紫棋","陈奕迅","薛之谦"}
  for i,v in ipairs(chips)do text(18+(i-1)*88,102,82,v,0,m.kb_row==-1 and m.kb_col==i-1 and C.accent or C.fg)end
  local suggestions=m.suggestions or{};local start=math.floor(((m.suggest_selected or 1)-1)/4)*4
  for slot=1,4 do local i=start+slot;if suggestions[i]then text(18+(slot-1)*112,123,106,suggestions[i],0,m.suggest_focused==1 and m.suggest_selected==i and C.accent or C.fg)end end
  -- Native input_search rows: 10 / 10 / 9 cells, including delete/space/search.
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
    text(48,y,400,s.name,0,col);text(48,y+17,400,s.artist or s.subtitle,0,C.dim);shown=shown+1
   end
  end
  if shown==0 then text(48,132,390,"暂无结果 · △ 编辑搜索",0,C.dim)end
 end
 bar(m.download_select_mode==1 and"□ 勾选   △ 下载选中   SELECT 退出多选"or"○ 打开   □ 下载   △ 编辑","L/R 搜索分类   SELECT 多选   × 返回")
end

local function xmbRows(t,m)
 local songs=m.songs or{};local selected=m.selected or 1
 header((m.list_title and m.list_title~="")and m.list_title or"音乐", "音乐 / 歌单 · "..tostring(songs.total or #songs).." 首",m)
 local first=math.max(1,selected-2);local shown=0
 for i,s in ipairs(songs)do local index=s.index or i;if index>=first and shown<4 then local y=90+shown*36
  if index==selected then text(25,y,20,">",0,C.accent)end
  text(50,y,400,s.name,0,index==selected and C.accent or C.fg);text(50,y+17,400,s.artist,0,C.dim);shown=shown+1
 end end
 if shown==0 then text(50,126,400,"暂无歌曲",0,C.dim)end
 bar(m.download_select_mode==1 and"□ 勾选   △ 下载选中   L 退出多选"or"○ 播放   □ 下载   △ 歌曲详情   L 多选","←→ 快移   × 返回")
end

local function xmbTransfers(t,m)
 local mv=m.download_tab==1;local rows=m.downloads or{};local selected=m.download_selected or 1
 header(mv and"下载 / MV"or"下载 / 歌曲",tostring(#rows).." 项 · 待解析 "..tostring(m.pending_downloads or 0),m)
 local states={[0]="等待中",[1]="下载中",[2]="已完成",[3]="失败",[4]="已暂停"}
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
