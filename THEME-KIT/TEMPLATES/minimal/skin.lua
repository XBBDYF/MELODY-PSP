-- 最小可用脚本主题模板
-- 说明：
--   1. 复制整个 minimal/ 目录，改 skin.json 的 id/name/author/version；
--   2. 只覆盖你需要改的页面，其它页面会自动回落原生界面；
--   3. 所有 m 字段都要兜底（or "" / or 0 / or {}）。

local C = {
  bg      = "#101418",
  panel   = "#171B21",
  panel2  = "#1E242C",
  text    = "#F2F5F8",
  sub     = "#B8C2CC",
  dim     = "#7C8894",
  line    = "#2C343E",
  accent  = "#4FC3A1",
  hot     = "#F07167",
}

local function text(x, y, w, s, font, color)
  ui.label(x, y, w, s, font or 0, color or C.text)
end

local function home(t, m)
  ui.rect(0, 0, 480, 272, C.bg)
  ui.rect(0, 0, 480, 40, C.panel)

  text(20, 10, 300, "我的主题", 2, C.accent)
  text(20, 60, 440, (m.song ~= nil and m.song ~= "") and m.song or "未在播放", 2, C.text)
  text(20, 96, 440, m.artist or "", 1, C.sub)

  if (m.paused or 0) == 1 then
    text(20, 128, 120, "已暂停", 0, C.dim)
  end

  if (app.option("show_hint") or 0) == 1 then
    text(20, 244, 440, ui.button_hint("○ 播放   △ 搜索   □ 设置   × 返回"), 0, C.dim)
  end
end

local function player(t, m)
  ui.rect(0, 0, 480, 272, C.bg)
  ui.rect(0, 0, 480, 36, C.panel)
  text(20, 8, 300, "正在播放", 1, C.sub)

  ui.cover(150, 56, 180, 180)
  text(20, 240, 440, (m.song ~= nil and m.song ~= "") and m.song or "未在播放", 1, C.text)
end

local function search(t, m)
  ui.rect(0, 0, 480, 272, C.bg)
  ui.rect(12, 30, 456, 30, C.panel2)
  text(22, 38, 420, (m.query ~= nil and m.query ~= "") and m.query or "搜索歌曲、歌手", 1,
       ((m.query or "") ~= "") and C.text or C.dim)
  text(20, 240, 440, ui.button_hint("○ 上屏   △ 联想   □ 语言   ← 删除"), 0, C.dim)
end

local skin = {
  pages = {
    home = home,
    player = player,
    search = search,
  },
  input = function(page, t, dt, m, k)
    -- 返回 false 表示不接管，交给原生处理（大部分情况这样最省事）
    return false
  end,
}

return skin
