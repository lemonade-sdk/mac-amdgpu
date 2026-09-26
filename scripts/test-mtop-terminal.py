#!/usr/bin/env python3
"""Exercise real terminal framing/protocols with synthetic data; no IOKit calls."""
import argparse, base64, fcntl, os, pathlib, pty, re, select, struct, subprocess, termios, time, zlib
p=argparse.ArgumentParser()
p.add_argument('--binary',default='build/amdgpu_mtop-candidate/amdgpu_mtop')
p.add_argument('--output',default='build/tests/mtop-terminal')
p.add_argument('--rows',type=int,default=42)
p.add_argument('--columns',type=int,default=110)
a=p.parse_args();out=pathlib.Path(a.output);out.mkdir(parents=True,exist_ok=True)
for protocol,program in [('kitty','kitty'),('iterm','iTerm.app'),('text','Apple_Terminal')]:
    master,slave=pty.openpty();fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',a.rows,a.columns,0,0))
    env=dict(os.environ,TERM='xterm-256color',TERM_PROGRAM=program,TERM_PROGRAM_VERSION='3.7.3')
    process=subprocess.Popen([a.binary,'--demo','--fast','--graphics',protocol],stdin=slave,stdout=slave,stderr=slave,env=env)
    os.close(slave);data=bytearray();start=time.monotonic();sent_h=sent_q=False
    try:
        while time.monotonic()-start<5:
            if select.select([master],[],[],.05)[0]:
                try:data.extend(os.read(master,1<<20))
                except OSError:break
            elapsed=time.monotonic()-start
            if elapsed>.25 and not sent_h:os.write(master,b'h');sent_h=True
            if elapsed>.85 and not sent_q:os.write(master,b'q');sent_q=True
            if process.poll() is not None:break
        assert process.wait(timeout=1)==0
    finally:
        if process.poll() is None:process.kill();process.wait()
        os.close(master)
    stream=bytes(data);(out/(protocol+'.ansi')).write_bytes(stream)
    assert b'DEMO (synthetic)' in stream
    assert b'Interval: 10ms' in stream
    assert b'\x1b[?25h' in stream and b'\x1b[?1049l' in stream
    assert stream.count(b'\x1b[2J')==1, 'full clear only on first frame'
    images=[]
    if protocol=='kitty':
        chunks=[]
        for control,payload in re.findall(rb'\x1b_G([^;\x1b]*);([^\x1b]*)\x1b\\',stream):
            assert len(payload)<=4096
            if b'a=T' in control:chunks=[]
            chunks.append(payload)
            if b'm=0' in control:images.append(base64.b64decode(b''.join(chunks),validate=True))
        assert len(images)>=2
    elif protocol=='iterm':
        images=[base64.b64decode(x,validate=True) for x in re.findall(rb'\x1b\]1337;File=[^:]*:([^\a]*)\a',stream)]
        assert len(images)>=2
    else:
        assert b'\x1b_G' not in stream and b'1337;File' not in stream
        assert any(0x2580<=ord(c)<=0x2588 for c in stream.decode())
    for i,png in enumerate(images[:2]):
        assert png[:8]==b'\x89PNG\r\n\x1a\n'
        (out/f'{protocol}-chart-{i}.png').write_bytes(png)
        at=8;compressed=bytearray();width=height=0
        while at<len(png):
            n=struct.unpack_from('>I',png,at)[0];kind=png[at+4:at+8];payload=png[at+8:at+8+n]
            assert zlib.crc32(kind+payload)==struct.unpack_from('>I',png,at+8+n)[0]
            if kind==b'IHDR':width,height=struct.unpack_from('>II',payload)
            if kind==b'IDAT':compressed.extend(payload)
            at+=12+n
        assert len(zlib.decompress(compressed))==(width*3+1)*height
    print(f'PASS {protocol}: keyboard toggle/quit, differential frame, UTF-8, {len(images)} pixel images; no GPU opened')
