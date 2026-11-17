-- Mock TI UI/ASI contracts; exercise the actual client and generated bundle.
package.path = './?.lua;' .. package.path
platform = {window = {invalidate=function() end, width=function() return 320 end,
    height=function() return 240 end, setBackgroundColor=function() end}}
local time = 0
timer = {getMilliSecCounter=function() time=time+100; return time end, start=function() end}
on = {}
local menu
local callbacks = {}
asi = {CONNECTED=1, ON=2, INVALID=3, BAUD_RATE_115200=115200,
    stopScanning=function() end, startScanning=function(cb) callbacks.scan=cb end,
    addStateListener=function(cb) callbacks.state=cb end}
package.preload.asi = function() return asi end
toolpalette = {register=function(m) menu=m end}
local writes, reading, baud, connected, listener, writeListener = {}, 0, nil, false, nil, nil
local deferred = false
local port = {
    setBaudRate=function(_, n) assert(not connected); baud=n end,
    connect=function(self, cb) connected=true; assert(baud==115200); cb(self, asi.CONNECTED) end,
    setReadListener=function(_, cb) listener=cb end,
    setWriteListener=function(_, cb) writeListener=cb end,
    setReadTimeout=function(_, n) assert(n>=30 and n<=3000) end,
    read=function(_, n) assert(n>=1 and n<=1024); reading=reading+1 end,
    write=function(self, s) writes[#writes+1]=s; if not deferred then writeListener(self) end end,
    getState=function() return 1 end,
    disconnect=function() connected=false end
}
assert(loadfile('build/camera_nc2.lua'))()
on.construction()
on.escapeKey() -- no nil port crash
callbacks.scan(port)
assert(writes[#writes]=='HELLO\n' and reading==1)
on.timer(); assert(reading==1) -- no overlapping reads
on.tabKey(); assert(writes[#writes]=='STREAM\n')
local lines = {}
for l in io.lines('build/fixtures.tsv') do lines[#lines+1]=l end
local seq, base, side, mode, checksum, payload = lines[17]:match('^(%d+)\t(%d+)\t(%d+)\t([KRDP])\t(%d+)\t([^\t]+)')
local wire = string.format('NC2 BEGIN %s %s %s %s %s %d %s\n',seq,base,side,side,mode,#payload,checksum)
for i=1,#payload,192 do wire=wire..string.format('NC2 DATA %s %d %s\n',seq,i-1,payload:sub(i,i+191)) end
wire=wire..'NC2 END '..seq..'\n'
port.getValue=function() return wire end
listener(port)
assert(writes[#writes]=='DONE '..seq..'\n' and reading==2)
local rectangles = 0
local gc = {setColorRGB=function() end, setFont=function() end, drawString=function() end,
    fillRect=function(_, x,y,w,h) assert(w>0 and h>0 and x>=0 and y>=0 and x+w<=320 and y+h<=240); rectangles=rectangles+1 end}
on.paint(gc); assert(rectangles==51) -- one horizontal run per solid row
on.escapeKey()
rectangles=0; on.paint(gc); assert(rectangles==51) -- stopped frame retained
on.charIn('p'); assert(writes[#writes]=='PIC\n')
on.charIn('f'); assert(writes[#writes]=='FLASH\n')
-- Real ASI writes complete asynchronously: SIZE must finish before STREAM.
deferred = true
on.tabKey(); assert(writes[#writes]=='WINDOW 2\n')
local before = #writes
on.timer(); assert(#writes==before)
writeListener(port); assert(writes[#writes]=='FORMAT 64\n')
writeListener(port); assert(writes[#writes]=='NOISE 8\n')
writeListener(port); assert(writes[#writes]=='SIZE 51\n')
writeListener(port); assert(writes[#writes]=='STREAM\n')
writeListener(port)
local originalWrite = port.write
local failed = false
port.write = function(self, text)
    if not failed then failed=true; return 'busy' end
    return originalWrite(self, text)
end
on.charIn('f'); before=#writes
on.timer(); assert(#writes==before+1 and writes[#writes]=='FLASH\n')
writeListener(port)
deferred = false
on.charIn('t'); assert(writes[#writes]=='METRICS 1\n'); on.paint(gc)
on.destroy(); assert(not connected)
print('TI client mock tests passed: connection, valid timeout/baud, one read in flight, stream, rendering, stop and photo')
