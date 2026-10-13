-- File bridge: runs Lua from cmd.lua, writes result to out.txt. Dev-only helper.
local dir = [[Z:\home\flx\Tools\mm7bridge\]]
local n = 0
function events.Tick()
	n = n + 1
	if n % 8 ~= 0 then return end
	local f = io.open(dir .. "cmd.lua", "rb")
	if not f then return end
	local src = f:read("*a"); f:close()
	os.remove(dir .. "cmd.lua")
	local fn, err = loadstring(src)
	local ok, res
	if fn then ok, res = pcall(fn) else ok, res = false, err end
	local o = io.open(dir .. "out.txt", "wb")
	o:write(ok and "OK " or "ERR ", tostring(res), "\n"); o:close()
end
