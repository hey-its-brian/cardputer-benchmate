import os, sys, time, termios, tty, zlib, struct, select
PORT=os.environ.get('BENCHMATE_PORT','/dev/cu.usbmodem101')
fd=os.open(PORT, os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
tty.setraw(fd)
def drain(t=0.2):
    end=time.time()+t; buf=b''
    while time.time()<end:
        r,_,_=select.select([fd],[],[],0.05)
        if r:
            try: buf+=os.read(fd,65536)
            except BlockingIOError: pass
    return buf
def keys(s, gap=0.25):
    for ch in s:
        os.write(fd, ch.encode()); time.sleep(gap)
def png(path,w,h,raw,scale=2):
    rows=[]
    for y in range(h):
        row=bytearray()
        for x in range(w):
            v=(raw[2*(y*w+x)]<<8)|raw[2*(y*w+x)+1]
            r=(v>>11)&31; g=(v>>5)&63; b=v&31
            px=bytes([(r*255)//31,(g*255)//63,(b*255)//31])
            row+=px*scale
        for _ in range(scale): rows.append(b'\x00'+bytes(row))
    def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
    data=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w*scale,h*scale,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows),9))+chunk(b'IEND',b'')
    open(path,'wb').write(data)
def shot(path):
    drain(0.1)
    os.write(fd,b'\x10')
    buf=b''; end=time.time()+4
    while time.time()<end:
        r,_,_=select.select([fd],[],[],0.1)
        if r:
            try: buf+=os.read(fd,65536)
            except BlockingIOError: pass
        i=buf.find(b'BMSHOT ')
        if i>=0:
            j=buf.find(b'\n',i)
            if j>=0:
                w,h=map(int,buf[i+7:j].split())
                if len(buf)-(j+1)>=w*h*2:
                    png(path,w,h,buf[j+1:j+1+w*h*2]); print('saved',path); return True
    print('FAILED',path,len(buf)); return False
out=sys.argv[1]
time.sleep(3); drain(0.3)
for step in sys.argv[2:]:
    kind,_,val=step.partition(':')
    if kind=='k': keys(val.replace('\\r','\r').replace('\\t','\t'))
    elif kind=='w': time.sleep(float(val))
    elif kind=='s': shot(os.path.join(out,val+'.png'))
os.close(fd)
