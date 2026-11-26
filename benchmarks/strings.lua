-- Benchmark: build n short strings, join them with a comma, split them back and
-- sum their lengths. Same algorithm as `strings.oskl`; the split is written by
-- hand because Lua has none.
local n = 5000
local parts = {}
for i = 0, n - 1 do
  parts[#parts + 1] = "item-" .. i
end

local text = table.concat(parts, ",")
local words = {}
local start = 1
while true do
  local at = string.find(text, ",", start, true)
  if at == nil then break end
  words[#words + 1] = string.sub(text, start, at - 1)
  start = at + 1
end
words[#words + 1] = string.sub(text, start)

local chars = 0
for _, w in ipairs(words) do chars = chars + #w end
print("strings " .. #text .. " " .. #words .. " " .. chars)
