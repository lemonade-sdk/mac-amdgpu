#!/usr/bin/env python3
"""Interpret the monitor's actual Kitty PTY frame as text + embedded-PNG HTML.
This is an offline protocol reconstruction, not a screenshot of a terminal app.
"""
import argparse,base64,html,json,pathlib,re
p=argparse.ArgumentParser();p.add_argument('capture');p.add_argument('--rows',type=int,default=42);p.add_argument('--columns',type=int,default=110)
a=p.parse_args();src=pathlib.Path(a.capture);text=src.read_bytes().decode();screen=[[' ']*a.columns for _ in range(a.rows)]
r=c=0;images={};pending=None;at=0
while at<len(text):
    if text.startswith('\x1b_G',at):
        end=text.index('\x1b\\',at);command=text[at+3:end];control,_,payload=command.partition(';')
        fields=dict(x.split('=',1) for x in control.split(',') if '=' in x)
        if fields.get('a')=='d':images.pop(fields.get('i'),None)
        elif fields.get('a')=='T':pending=dict(row=r,column=c,rows=int(fields['r']),columns=int(fields['c']),data='',id=fields['i'])
        if pending is not None and payload:
            pending['data']+=payload
            if fields.get('m')=='0':images[pending['id']]=pending;pending=None
        at=end+2;continue
    if text.startswith('\x1b[',at):
        match=re.match(r'\x1b\[([0-9;?]*)([A-Za-z])',text[at:]);assert match
        arg,op=match.groups();nums=[int(x) if x else 0 for x in arg.split(';')] if not arg.startswith('?') else []
        if op=='H':r=max(0,(nums[0] if nums else 1)-1);c=max(0,(nums[1] if len(nums)>1 else 1)-1)
        if op=='K' and r<a.rows:screen[r][c:]=[' ']*(a.columns-c)
        if op=='J' and nums==[2]:screen=[[' ']*a.columns for _ in range(a.rows)]
        at+=len(match.group());continue
    ch=text[at];at+=1
    if ch=='\r':c=0
    elif ch=='\n':r+=1
    elif ch>=' ':
        if r<a.rows and c<a.columns:screen[r][c]=ch
        c+=1
lines=[''.join(row).rstrip() for row in screen]
# Text preserves exact terminal text; annotate pixel rectangles separately.
notes=['', 'Pixel placements (zero-based; exact PNGs embedded in HTML):']
for image in images.values():notes.append(f"  image {image['id']}: row {image['row']}, col {image['column']}, {image['columns']}x{image['rows']} cells")
base=src.with_suffix('');base.with_suffix('.frame.txt').write_text('\n'.join(f'{i+1:02d} | {line}' for i,line in enumerate(lines))+'\n'+'\n'.join(notes)+'\n')
parts=['<!doctype html><meta charset="utf-8"><title>Captured amdgpu_mtop frame</title>',
'<body style="background:#080e17;color:#d8e4ef"><p>Offline reconstruction of the actual synthetic PTY frame; not a native terminal screenshot.</p>',
f'<div style="position:relative;background:#0f1723;font:14px/20px monospace;width:{a.columns}ch;height:{a.rows*20}px">']
for i,line in enumerate(lines):parts.append(f'<pre style="position:absolute;left:0;top:{i*20}px;margin:0;font:inherit">{html.escape(line)}</pre>')
for image in images.values():parts.append(f'<img alt="captured pixel chart" style="position:absolute;left:{image["column"]}ch;top:{image["row"]*20}px;width:{image["columns"]}ch;height:{image["rows"]*20}px" src="data:image/png;base64,{image["data"]}">')
parts.append('</div>');base.with_suffix('.frame.html').write_text(''.join(parts))
base.with_suffix('.frame.json').write_text(json.dumps(dict(lines=lines,images=list(images.values()),rows=a.rows,columns=a.columns)))
print(base.with_suffix('.frame.txt'));print(base.with_suffix('.frame.html'))
