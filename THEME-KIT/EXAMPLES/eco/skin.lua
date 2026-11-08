-- ECO 省电原型（验证用）
-- 目标：把每帧的绘制操作压到最少，用来测量「主题能省多少 CPU」。
-- 手法：
--   1. 整屏只铺 1 个矩形（1 次 fill，而不是渐变/多层叠加）；
--   2. 每页最多 4 行文字；
--   3. 不画封面、不画唱片、不画波浪、不用 ui.gradient / ui.overlay；
--   4. 不做逐帧动画（t 不参与任何绘制）；
--   5. 不引用任何图片/音效资源（少一次存储读取）。
-- 说明：PSP 是 LCD + 常亮背光，纯黑背景并不省电；省电来自「每帧少干活」。

local C = {
  bg     = "#000000",
  text   = "#DDDDDD",
  sub    = "#AAAAAA",
  dim    = "#777777",
  accent = "#66CC99",
}

local function line(x, y, w, s, font, color)
  ui.label(x, y, w, s or "", font or 0, color or C.text)
end

local function header(title, m)
  line(16, 10, 380, title, 1, C.accent)
  line(400, 12, 64, (m.clock or ""), 0, C.dim)
  line(16, 34, 448, "", 0, C.dim)
end

local function home(t, m)
  ui.rect(0, 0, 480, 272, C.bg)
  header("MELODY · ECO", m)
  line(16, 70, 448, (m.song ~= nil and m.song ~= "") and m.song or "未播放", 2, C.text)
  line(16, 108, 448, m.artist or "", 1, C.sub)
  line(16, 150, 448, (m.wlan_enabled or 0) == 1 and "网络已连接" or "离线", 0, C.dim)
  line(16, 244, 448, "○ 播放   △ 搜索   □ 设置   × 返回", 0, C.dim)
end

local function player(t, m)
  ui.rect(0, 0, 480, 272, C.bg)
  header("正在播放", m)
  line(16, 80, 448, (m.song ~= nil and m.song ~= "") and m.song or "未播放", 2, C.text)
  line(16, 118, 448, m.artist or "", 1, C.sub)
  line(16, 156, 448, (m.paused or 0) == 1 and "已暂停" or "播放中", 0, C.accent)
  line(16, 244, 448, "○ 暂停   ← → 切歌   × 返回", 0, C.dim)
end

local function search(t, m)
  ui.rect(0, 0, 480, 272, C.bg)
  header("搜索", m)
  line(16, 56, 448, (m.query ~= nil and m.query ~= "") and m.query or "输入拼音/假名", 1,
       ((m.query or "") ~= "") and C.text or C.dim)
  local n = 0
  local songs = m.songs or {}
  for i = 1, math.min(#songs, 4) do
    local s = songs[i]
    if s ~= nil then
      line(16, 96 + n * 28, 448, (s.name or "") .. "  " .. (s.artist or ""), 0, C.text)
      n = n + 1
    end
  end
  line(16, 244, 448, "○ 上屏   △ 联想   □ 语言", 0, C.dim)
end

return {
  pages = { home = home, player = player, search = search },
  input = function(page, t, dt, m, k)
    return false
  end,
}
