-- Authoritative NC2 camera client source. tools/build.py embeds camera_protocol.lua.
platform.apiLevel = '2.7'
local protocol = require('camera_protocol')
require 'asi'

local port, receiver
local frame, runs, frameWidth, frameHeight = nil, {}, 0, 0
local running, snapshot, readPending = false, false, false
local selectedSize, status, fps, lastFrameTime = 72, 'Connect the ESP32-CAM adapter', 0, nil
local palette = {}
for i = 0, 255 do
    palette[i] = {math.floor(math.floor(i / 32) * 255 / 7 + 0.5),
                  math.floor((math.floor(i / 4) % 8) * 255 / 7 + 0.5), (i % 4) * 85}
end

local function invalidate() platform.window:invalidate() end
local writes, writeBusy = {}, false
local function pumpWrites()
    if not port or writeBusy or #writes == 0 then return end
    writeBusy = true
    local err = port:write(writes[1])
    if err then writeBusy = false; status = 'Serial write: ' .. tostring(err) end
end

local function send(command)
    if not port then status = 'No serial port connected'; return false end
    -- Duplicate ACKs may be requested after a retry; one queued copy is enough.
    for _, queued in ipairs(writes) do if queued == command then return true end end
    if #writes >= 16 then status = 'Serial write queue full'; return false end
    writes[#writes + 1] = command
    pumpWrites()
    return true
end

local function clearQueuedWrites()
    -- Preserve the in-flight entry until its completion callback removes it.
    writes = writeBusy and {writes[1]} or {}
end

local function onWrite(p, err)
    if p ~= port then return end
    writeBusy = false
    if err then status = 'Serial write: ' .. tostring(err)
    else table.remove(writes, 1); pumpWrites() end
end

local function receivedFrame(pixels, w, h, mode, bytes)
    -- Cache horizontal runs ONCE per complete frame; no per-pixel tables in paint.
    local nextRuns = {}
    for y = 0, h - 1 do
        local x = 0
        while x < w do
            local value = pixels:byte(y * w + x + 1)
            local stop = x + 1
            while stop < w and pixels:byte(y * w + stop + 1) == value do stop = stop + 1 end
            nextRuns[#nextRuns + 1] = {x, stop, y, value}
            x = stop
        end
    end
    frame, runs, frameWidth, frameHeight = pixels, nextRuns, w, h
    local now = timer.getMilliSecCounter()
    if lastFrameTime and now > lastFrameTime then fps = 1000 / (now - lastFrameTime) end
    lastFrameTime = now
    status = string.format('%dx%d  RGB332  %s  %d B', w, h, mode, bytes)
    if snapshot then snapshot = false end
    invalidate()
end

receiver = protocol.new(send, receivedFrame, function(message)
    status = message
    invalidate()
end)

local function requestRead()
    if not port or readPending then return end
    -- One read at a time. Re-arm promptly even when no bytes arrive.
    readPending = true
    local err = port:read(512)
    if err then readPending = false; status = 'Serial read: ' .. tostring(err) end
end

local function onRead(p, err)
    if p ~= port then return end
    readPending = false
    local ok, data = pcall(p.getValue, p)
    if not err and ok and data and #data > 0 then receiver:feed(data) end
    -- Re-arm from the callback instead of waiting for the old 100 ms polling tick.
    requestRead()
end

local function stop()
    running, snapshot, lastFrameTime = false, false, nil
    clearQueuedWrites()
    send('STOP\n')
    receiver:reset()
    status = 'Stopped; last complete frame retained'
    invalidate()
end

local function start(single)
    if not port then status = 'No serial port connected'; invalidate(); return end
    clearQueuedWrites()
    receiver:reset()
    running, snapshot = not single, single
    fps, lastFrameTime = 0, nil
    send('SIZE ' .. selectedSize .. '\n')
    send(single and 'PIC\n' or 'STREAM\n')
    status = single and 'Capturing photo...' or 'Starting stream...'
    requestRead()
    invalidate()
end

local function chooseSize(size)
    selectedSize = size
    if running or snapshot then start(snapshot)
    else status = 'Selected ' .. size .. ' x ' .. size; invalidate() end
end

local function connect(p, event)
    if event ~= asi.CONNECTED then return end
    asi.stopScanning()
    port, readPending = p, false
    writes, writeBusy = {}, false
    receiver:reset()
    port:setReadListener(onRead)
    port:setWriteListener(onWrite)
    port:setReadTimeout(30)
    status = 'Connected. Tab: stream; P: photo; Esc: stop'
    send('HELLO\n')
    requestRead()
    invalidate()
end

local function scan(p)
    if not port then
        p:setBaudRate(asi.BAUD_RATE_115200)
        p:connect(connect)
    end
end

function on.construction()
    platform.window:setBackgroundColor(0xFFFFFF)
    asi.addStateListener(function(state)
        if state == asi.ON then asi.startScanning(scan) end
    end)
    asi.startScanning(scan)
    timer.start(0.05)
end

function on.timer()
    if port and port:getState() == asi.INVALID then
        port, readPending, running, snapshot = nil, false, false, false
        writes, writeBusy = {}, false
        receiver:reset()
        status = 'Disconnected; reconnect the adapter'
        asi.startScanning(scan)
        invalidate()
    else pumpWrites(); requestRead() end
end

function on.paint(gc)
    local w, h = platform.window:width(), platform.window:height()
    gc:setColorRGB(0, 0, 0)
    gc:setFont('sansserif', 'r', 8)
    gc:drawString(string.format('NC2  %.1f FPS  |  Tab: stream  P: photo', fps), 3, 0)
    if frame then
        local size = math.min(204, w - 8, h - 35)
        local left, top = math.floor((w - size) / 2), 17
        local lastColour = -1
        for _, run in ipairs(runs) do
            if run[4] ~= lastColour then
                local rgb = palette[run[4]]
                gc:setColorRGB(rgb[1], rgb[2], rgb[3])
                lastColour = run[4]
            end
            local x1 = math.floor(run[1] * size / frameWidth)
            local x2 = math.floor(run[2] * size / frameWidth)
            local y1 = math.floor(run[3] * size / frameHeight)
            local y2 = math.floor((run[3] + 1) * size / frameHeight)
            gc:fillRect(left + x1, top + y1, x2 - x1, y2 - y1)
        end
    else gc:drawString('Menu: select quality, then start camera', 8, 55) end
    gc:setColorRGB(0, 0, 0)
    gc:setFont('sansserif', 'r', 7)
    gc:drawString(status, 3, h - 14)
end

function on.tabKey() start(false) end
function on.escapeKey() stop() end
function on.charIn(key)
    if key:lower() == 'p' then start(true)
    elseif key:lower() == 'f' then send('FLASH\n') end
end
function on.destroy()
    if port then send('STOP\n'); port:disconnect(); port = nil end
end

toolpalette.register({
    {'Camera', {'Start stream', function() start(false) end},
               {'Take photo', function() start(true) end},
               {'Stop', stop}, {'Toggle flash', function() send('FLASH\n') end}},
    {'Quality', {'Fast: 51 x 51', function() chooseSize(51) end},
                {'Balanced: 72 x 72', function() chooseSize(72) end},
                {'Detail: 102 x 102', function() chooseSize(102) end}}
})
