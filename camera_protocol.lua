-- NC2 wire receiver; Lua 5.1 compatible, no TI API dependencies.
local M = {}
local floor, byte, char = math.floor, string.byte, string.char
local MAX_SIDE, CHUNK, MAX_LINE = 102, 192, 320
local MAX_WIRE = 4 * math.ceil(MAX_SIDE * MAX_SIDE / 3)
local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local lookup = {}
for i = 1, #alphabet do lookup[byte(alphabet, i)] = i - 1 end
local xor4 = {}
for a = 0, 15 do
    xor4[a] = {}
    for b = 0, 15 do
        local x, y, value, power = a, b, 0, 1
        for _ = 1, 4 do
            if x % 2 ~= y % 2 then value = value + power end
            x, y, power = floor(x / 2), floor(y / 2), power * 2
        end
        xor4[a][b] = value
    end
end

function M.adler32(data)
    local a, b = 1, 0
    for i = 1, #data do
        a = (a + byte(data, i)) % 65521
        b = (b + a) % 65521
    end
    return b * 65536 + a
end

function M.base64decode(data)
    if #data % 4 ~= 0 then return nil end
    local out = {}
    for i = 1, #data, 4 do
        local a, b = lookup[byte(data, i)], lookup[byte(data, i + 1)]
        local c, d = lookup[byte(data, i + 2)], lookup[byte(data, i + 3)]
        local padC, padD = data:sub(i + 2, i + 2) == '=', data:sub(i + 3, i + 3) == '='
        if not a or not b or (not c and not padC) or (not d and not padD) or
           (padC and not padD) or ((padC or padD) and i + 3 ~= #data) then return nil end
        if (padC and b % 16 ~= 0) or (padD and not padC and c % 4 ~= 0) then return nil end
        local v = a * 262144 + b * 4096 + (c or 0) * 64 + (d or 0)
        out[#out + 1] = char(floor(v / 65536))
        if not padC then out[#out + 1] = char(floor(v / 256) % 256) end
        if not padD then out[#out + 1] = char(v % 256) end
    end
    return table.concat(out)
end

function M.unpack(data, expected)
    local out, size, i = {}, 0, 1
    while i <= #data do
        local token = byte(data, i)
        i = i + 1
        if token == 128 then return nil end
        local count = token < 128 and token + 1 or 257 - token
        if size + count > expected then return nil end
        if token < 128 then
            if i + count - 1 > #data then return nil end
            out[#out + 1] = data:sub(i, i + count - 1)
            i = i + count
        else
            if i > #data then return nil end
            out[#out + 1] = string.rep(data:sub(i, i), count)
            i = i + 1
        end
        size = size + count
    end
    if size ~= expected then return nil end
    return table.concat(out)
end

function M.paletteUnpack(data, expected)
    if #data < 2 then return nil end
    local colours = byte(data, 1) + 1
    local bits = 1
    while 2 ^ bits < colours do bits = bits + 1 end
    local first = colours + 2
    if #data ~= colours + 1 + math.ceil(expected * bits / 8) then return nil end
    local out, buffer, count, index = {}, 0, 0, first
    for i = 1, expected do
        if count < bits then
            buffer = buffer * 256 + byte(data, index)
            count, index = count + 8, index + 1
        end
        count = count - bits
        local value = floor(buffer / 2 ^ count)
        buffer = buffer % 2 ^ count
        if value >= colours then return nil end
        out[i] = data:sub(value + 2, value + 2)
    end
    if buffer ~= 0 then return nil end -- canonical padding
    return table.concat(out)
end

function M.applyDelta(delta, previous)
    if #delta ~= #previous then return nil end
    local out = {}
    for i = 1, #delta do
        local a, b = byte(delta, i), byte(previous, i)
        out[i] = char(xor4[a % 16][b % 16] + 16 * xor4[floor(a / 16)][floor(b / 16)])
    end
    return table.concat(out)
end

local Receiver = {}
Receiver.__index = Receiver
function M.new(send, frame, status)
    return setmetatable({send = send, frame = frame, status = status or function() end,
        window = 1, clock = function() return 0 end, decodeMs = 0, line = "", dropping = false, current = nil, previous = nil, lastSequence = 0}, Receiver)
end

function Receiver:reset()
    self.line, self.dropping, self.current = "", false, nil
    self.previous, self.lastSequence = nil, 0
end

function Receiver:reject(reason, sequence)
    local seq = sequence or (self.current and self.current.seq)
    if seq then self.send("NAK " .. string.format("%.0f", seq) .. "\n") end
    self.current = nil
    self.status("Frame rejected: " .. reason)
end

local function validSequence(seq)
    return seq and seq >= 1 and seq <= 4294967295
end

function Receiver:process(line)
    local s, base, w, h, mode, length, checksum, suffix = line:match(
        "^NC2 BEGIN (%d+) (%d+) (%d+) (%d+) ([KRDP]) (%d+) (%d+)(.*)$")
    if s then
        local profile = suffix == '' and 256 or tonumber(suffix:match('^ (%d+)$'))
        s, base, w, h = tonumber(s), tonumber(base), tonumber(w), tonumber(h)
        length, checksum = tonumber(length), tonumber(checksum)
        if (profile~=16 and profile~=64 and profile~=256) or not validSequence(s) or base > 4294967295 or w < 1 or w > MAX_SIDE or h ~= w or
           length < 4 or length > 4 * math.ceil(w * h / 3) or length % 4 ~= 0 or
           checksum > 4294967295 or (mode ~= 'D' and base ~= 0) then
            self:reject("header", validSequence(s) and s or nil)
            return
        end
        if s == self.lastSequence then return end -- first packet/DONE acknowledgement retry
        local c = self.current
        if c and c.seq == s then
            if c.profile ~= profile or c.base ~= base or c.w ~= w or c.mode ~= mode or c.length ~= length or c.checksum ~= checksum then
                self:reject("conflicting header")
            end
            return
        end
        if mode == 'D' and (not self.previous or base ~= self.lastSequence or
                           profile ~= self.profile or w ~= self.width or h ~= self.height) then
            self:reject("missing delta reference", s)
            return
        end
        self.current = {profile = profile, seq = s, base = base, w = w, h = h, mode = mode,
                        length = length, checksum = checksum, parts = {}, size = 0}
        return
    end
    local seq, offset, data = line:match("^NC2 DATA (%d+) (%d+) ([A-Za-z0-9+/=]+)$")
    if seq then
        seq, offset = tonumber(seq), tonumber(offset)
        local c = self.current
        if not c or c.seq ~= seq then return end
        if #data > CHUNK or #data % 4 ~= 0 or offset + #data > c.length then
            self:reject("chunk length")
            return
        end
        if offset == c.size then
            c.parts[#c.parts + 1] = data
            c.size = c.size + #data
        elseif not (offset < c.size and offset + #data <= c.size) then
            self:reject("chunk offset")
            return
        end
        if c.size % (CHUNK*self.window)==0 or c.size==c.length then
            self.send(string.format("ACK %.0f %d\n", seq, c.size))
        end
        return
    end
    seq = tonumber(line:match("^NC2 END (%d+)$"))
    if seq then
        if seq == self.lastSequence then
            self.send(string.format("DONE %.0f\n", seq))
            return
        end
        local c = self.current
        if not c or c.seq ~= seq then return end
        if c.size ~= c.length then self:reject("incomplete payload"); return end
        local decodeStarted = self.clock()
        local payload = M.base64decode(table.concat(c.parts))
        local pixels = payload
        if payload and c.mode == 'P' then pixels = M.paletteUnpack(payload, c.w * c.h)
        elseif payload and c.mode ~= 'K' then pixels = M.unpack(payload, c.w * c.h) end
        if pixels and c.mode == 'D' then pixels = M.applyDelta(pixels, self.previous) end
        if not pixels or #pixels ~= c.w * c.h or M.adler32(pixels) ~= c.checksum then
            self:reject("decode/checksum")
            return
        end
        if c.profile==16 then
            for i=1,#pixels do if pixels:byte(i)>15 then self:reject('grayscale index'); return end end
        end
        self.decodeMs = self.clock()-decodeStarted
        self.profile = c.profile
        self.previous, self.lastSequence = pixels, seq
        self.width, self.height, self.current = c.w, c.h, nil
        -- Frame callback also prepares the render cache before granting another frame.
        self.frame(pixels, c.w, c.h, c.mode, c.length, c.profile)
        self.send(string.format("DONE %.0f\n", seq))
        return
    end
    local drop = tonumber(line:match('^NC2 DROP (%d+)$'))
    if drop then
        if self.current and self.current.seq==drop then self.current=nil end
        return
    end
    local id, cap, proc, transfer, drops = line:match('^NC2 STATS (%d+) (%d+) (%d+) (%d+) (%d+)$')
    if id then
        self.remoteStats = {capture=tonumber(cap), process=tonumber(proc), transfer=tonumber(transfer), dropped=tonumber(drops)}
        return
    end
    if line:match("^NC2 READY ") then self:reset(); self.status(line)
    elseif line:match("^NC2 ERROR ") then self.current = nil; self.status(line)
    elseif line:match("^NC2 STATUS ") then self.status(line)
    elseif line:match("^NC2 ") then self:reject("malformed packet") end
end

function Receiver:feed(data)
    -- Serial callbacks may split ANY marker or deliver multiple lines at once.
    local start = 1
    while start <= #data do
        local stop = data:find('\n', start, true)
        local part = data:sub(start, stop and stop - 1 or #data)
        if not self.dropping then
            if #self.line + #part > MAX_LINE then
                self.line, self.dropping = "", true
                self:reject("line overflow")
            else self.line = self.line .. part end
        end
        if stop then
            if not self.dropping then self:process((self.line:gsub('\r$', ''))) end
            self.line, self.dropping = "", false
            start = stop + 1
        else break end
    end
end
M.MAX_WIRE, M.CHUNK = MAX_WIRE, CHUNK
return M
