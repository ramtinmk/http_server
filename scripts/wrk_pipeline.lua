-- Pipelined requests for wrk: send `depth` requests per socket write.
--
-- Usage:
--   wrk -t4 -c100 -d10s --latency -s scripts/wrk_pipeline.lua \
--       http://127.0.0.1:8081/home 8
--
-- The trailing argument (8 above) is the pipeline depth. The server processes
-- at most MAX_PIPELINE_DEPTH (16) requests per recv() pass.
local depth = 8

init = function(args)
    if args[1] then depth = tonumber(args[1]) end
    local parts = {}
    for i = 1, depth do
        parts[i] = wrk.format("GET", "/home")
    end
    req = table.concat(parts)
end

request = function()
    return req
end
