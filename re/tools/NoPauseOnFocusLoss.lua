-- Keep MM7 running when its window loses focus (dev convenience).
-- aWinProc's WM_ACTIVATEAPP handler: deactivate branch at 0x463BAA sets flag 0x100 at 0x6BE1E4,
-- pauses timers/music and makes Game_Loop sleep in WaitMessage; activate branch at 0x463A63
-- undoes it (no-op if not paused). Under Wine the GetForegroundWindow check can route even a
-- re-activation into the deactivate branch, so send every WM_ACTIVATEAPP to the activate branch.
-- Both branches run with ebx = 0 (set at 0x463A3D).
if mmver == 7 then
	mem.asmpatch(0x463BAA, "jmp 0x463A63 - 0x463BAA", 5)
end
