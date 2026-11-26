-- Benchmark: append n integers to an array, then sum them. Same algorithm as
-- `arrays.oskl`.
local n = 200000
local xs = {}
for i = 0, n - 1 do
  xs[#xs + 1] = i
end

local sum = 0
for _, x in ipairs(xs) do sum = sum + x end
print("arrays " .. sum .. " " .. #xs)
