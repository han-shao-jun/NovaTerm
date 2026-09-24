-- Clink 在每次进入新的命令编辑提示符时调用一次，避免异步 prompt
-- 过滤器重绘时重复发布就绪事件。仅向终端输出内部 OSC 标记。
local prompt_generation = 0

clink.onbeginedit(function()
    prompt_generation = prompt_generation + 1
    local exit_code = os.geterrorlevel()
    clink.print("\x1b]633;NT;PROMPT;" .. prompt_generation .. ";"
        .. exit_code .. "\x07", NONL)
end)
