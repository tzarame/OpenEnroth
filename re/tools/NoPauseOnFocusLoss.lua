-- Keep MM7 running when its window loses focus (dev convenience).
-- aWinProc's WM_ACTIVATEAPP "deactivate" branch (0x463BAA) sets flag 0x100 at 0x6BE1E4,
-- pauses timers/music and makes Game_Loop sleep in WaitMessage. Under Wine the branch is
-- also taken with wParam=1 (GetForegroundWindow check), so skip the branch itself: jump to "return 0".
if mmver == 7 then
	mem.asmpatch(0x463BAA, "jmp absolute 0x4639FE")
end
