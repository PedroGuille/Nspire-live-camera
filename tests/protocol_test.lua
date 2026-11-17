package.path = './?.lua;' .. package.path
local p = require 'camera_protocol'
local function hex(data)
    return (data:gsub('.', function(c) return string.format('%02x', c:byte()) end))
end
local fixture = {}
for line in io.lines('build/fixtures.tsv') do
    local seq, base, side, mode, checksum, payload, expected = line:match('^(%d+)\t(%d+)\t(%d+)\t([KRDP])\t(%d+)\t([^\t]+)\t(.+)$')
    assert(seq)
    fixture[#fixture + 1] = {seq=tonumber(seq), base=tonumber(base), side=tonumber(side),
        mode=mode, checksum=checksum, payload=payload, expected=expected}
end
local function begin(f)
    return string.format('NC2 BEGIN %d %d %d %d %s %d %s\n',
        f.seq, f.base, f.side, f.side, f.mode, #f.payload, f.checksum)
end
local function packets(f)
    local out = {begin(f)}
    for offset = 0, #f.payload - 1, p.CHUNK do
        out[#out+1] = string.format('NC2 DATA %d %d %s\n', f.seq, offset, f.payload:sub(offset+1, offset+p.CHUNK))
    end
    out[#out+1] = 'NC2 END ' .. f.seq .. '\n'
    return out
end
-- Same C++-encoded frames delivered bytewise, in fragments and coalesced.
for _, chunkSize in ipairs({1, 7, 191, 512, 1000000}) do
    local replies, received = {}, 0
    local current
    local r = p.new(function(s) replies[#replies+1] = s end, function(data, w, h, mode)
        assert(hex(data) == current.expected and w == current.side and h == w and mode == current.mode)
        received = received + 1
    end)
    for _, f in ipairs(fixture) do
        current = f
        local wire = table.concat(packets(f))
        for i = 1, #wire, chunkSize do r:feed(wire:sub(i, i+chunkSize-1)) end
        assert(replies[#replies] == 'DONE ' .. f.seq .. '\n')
    end
    assert(received == #fixture)
end
assert(p.adler32('Wikipedia') == 0x11e60398)
for _, bad in ipairs({'A', 'AAA', '====', '=AAA', 'A=AA', 'AA=A', 'AB==', 'AAB=', 'AA==AAAA', 'AA?A'}) do
    assert(not p.base64decode(bad), bad)
end
assert(not p.paletteUnpack(string.char(0, 10, 128), 1)) -- invalid palette index
assert(not p.paletteUnpack(string.char(0, 10, 1), 1)) -- nonzero padding
assert(not p.paletteUnpack(string.char(1, 10), 1)) -- truncated palette
assert(p.base64decode('AAEC/f7/') == string.char(0,1,2,253,254,255))
for _, v in ipairs({string.char(128), string.char(255), string.char(127,0), string.char(255,0)}) do
    assert(not p.unpack(v, 1))
end
-- Lost ACK causes a duplicate packet: acknowledge again without duplicating pixels.
local replies, count = {}, 0
local r = p.new(function(s) replies[#replies+1] = s end, function() count=count+1 end)
local f = fixture[18] -- 51x51 key frame, multiple packets
assert(f.mode ~= 'D')
local lines = packets(f)
r:feed(lines[1] .. lines[2])
local ack = replies[#replies]
r:feed(lines[1] .. lines[2])
assert(replies[#replies] == ack)
for i=3,#lines do r:feed(lines[i]) end
assert(count==1 and replies[#replies]=='DONE '..f.seq..'\n')
r:feed(lines[#lines]) -- lost DONE
assert(count==1 and replies[#replies]=='DONE '..f.seq..'\n')
-- Bad checksum must leave the last valid frame/reference intact.
local previous = r.previous
local damaged = table.concat(lines):gsub('BEGIN '..f.seq..' ', 'BEGIN 900 '):gsub('DATA '..f.seq..' ', 'DATA 900 '):gsub('END '..f.seq, 'END 900')
damaged = damaged:gsub('([A-Za-z0-9+/=]+)\nNC2 END', function(s)
    return (s:sub(1,1)=='A' and 'B' or 'A')..s:sub(2)..'\nNC2 END'
end)
r:feed(damaged)
assert(count==1 and r.previous==previous and replies[#replies]=='NAK 900\n')
-- Rejection and recovery from missing base, huge headers/lines, malformed data.
r:reset()
r:feed('NC2 BEGIN 901 55 72 72 D 4 1\n')
assert(replies[#replies]=='NAK 901\n')
r:feed('NC2 BEGIN 902 0 9999 9999 K 999999999 1\n')
assert(not r.current)
r:feed('NC2 BEGIN 903 0 72 72 K 100 1\n' .. string.rep('x', 20000))
assert(#r.line <= 320 and not r.current)
r:feed('\n' .. table.concat(lines))
assert(count==2)
r:reset()
r:feed(lines[1] .. 'NC2 DATA '..f.seq..' 192 AAAA\n')
assert(replies[#replies]=='NAK '..f.seq..'\n')
r:feed(table.concat(lines))
assert(count==3)
r:feed('NC2 READY 2 RGB332 51 72 102\r\n')
assert(not r.previous and r.lastSequence==0)
print('Lua protocol tests passed: 200 cross-language decodes, fragmentation, retries, corruption, bounds and recovery')
-- Reduced-colour C++ frames, grouped ACKs, duplicate bursts, dropped frames,
-- format changes and timing telemetry. Both endpoints still validate checksums.
for _, fragment in ipairs({1, 191, 1024}) do
    local replies, count, expected, profile = {}, 0, nil, nil
    local tick=0
    local r=p.new(function(s) replies[#replies+1]=s end,function(pixels,w,h,mode,n,fmt)
        assert(hex(pixels)==expected and fmt==profile)
        count=count+1
    end)
    r.window=2
    r.clock=function() tick=tick+3; return tick end
    for line in io.lines('build/profiles.tsv') do
        local seq,base,side,mode,checksum,payload,pixels,fmt=line:match('^(%d+)\t(%d+)\t(%d+)\t([KRDP])\t(%d+)\t([^\t]+)\t([^\t]+)\t(%d+)$')
        expected,profile=pixels,tonumber(fmt)
        local f={seq=tonumber(seq),base=tonumber(base),side=tonumber(side),mode=mode,checksum=checksum,payload=payload}
        local lines=packets(f)
        lines[1]=lines[1]:gsub('\n$', ' '..fmt..'\n')
        for _,packet in ipairs(lines) do
            for i=1,#packet,fragment do r:feed(packet:sub(i,i+fragment-1)) end
        end
        assert(replies[#replies]=='DONE '..seq..'\n' and r.decodeMs==3)
    end
    assert(count==9)
    r:feed('NC2 STATS 1009 12 7 99 2\n')
    assert(r.remoteStats.capture==12 and r.remoteStats.transfer==99 and r.remoteStats.dropped==2)
    r:feed('NC2 BEGIN 2000 0 51 51 K 3468 1 64\n')
    r:feed('NC2 DATA 2000 0 '..string.rep('A',192)..'\n')
    local n=#replies
    r:feed('NC2 DATA 2000 0 '..string.rep('A',192)..'\n')
    assert(#replies==n) -- wait for second chunk
    r:feed('NC2 DATA 2000 192 '..string.rep('A',192)..'\n')
    assert(replies[#replies]=='ACK 2000 384\n')
    r:feed('NC2 DATA 2000 192 '..string.rep('A',192)..'\n')
    assert(replies[#replies]=='ACK 2000 384\n')
    local previous=r.previous
    r:feed('NC2 DROP 2000\n')
    assert(not r.current and r.previous==previous)
end
print('Low-lag protocol tests passed: reduced-colour profiles, grouped ACK loss, drop recovery and stage timings')
