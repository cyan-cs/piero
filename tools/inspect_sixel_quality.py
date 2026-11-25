"""Independent decoder for benchmark output; save quantized PNG and PSNR.
Pillow is only a developer fixture/inspection dependency, never a product one.
"""
from pathlib import Path
import sys,re,math,csv
from PIL import Image,ImageChops,ImageStat,ImageDraw
root=Path(sys.argv[1]); rows=[]
def decode(data):
    m=re.match(rb'\x1bP0;1;0q"1;1;(\d+);(\d+)',data)
    if not m: raise ValueError('header')
    w,h=map(int,m.groups()); assert 0<w<=16384 and 0<h<=16384 and w*h<=2097152
    im=Image.new('RGB',(w,h));p=m.end();pal={};c=x=y=0;painting=False
    while data[p:p+2]!=b'\x1b\\':
        ch=data[p];p+=1
        if ch==ord('#'):
            m=re.match(rb'(\d+)(?:;2;(\d+);(\d+);(\d+))?',data[p:]);assert m
            c=int(m[1]);assert c<256;p+=m.end()
            if m[2] is not None:
                assert not painting and c not in pal
                pal[c]=tuple((int(m[k])*255+50)//100 for k in (2,3,4))
            continue
        if ch==ord('$'):x=0;continue
        if ch==ord('-'):x=0;y+=6;continue
        count=1
        if ch==ord('!'):
            m=re.match(rb'(\d+)',data[p:]);assert m
            count=int(m[1]);p+=m.end();ch=data[p];p+=1
        assert 63<=ch<=126 and count<=w
        painting=True
        for dx in range(count):
            assert x+dx<w
            for dy in range(6):
                if ch-63 & 1<<dy:
                    assert y+dy<h
                    im.putpixel((x+dx,y+dy),pal[c])
        x+=count
    assert p+2==len(data)
    return im,len(pal)
for f in root.glob('*.sixel'):
    stem=f.name.removesuffix('.sixel')
    base=stem.removesuffix('.balanced').removesuffix('.fast')
    src=Image.open(root/(base+'.ppm')).convert('RGB')
    im,colors=decode(f.read_bytes()); assert src.size==im.size
    diff=ImageChops.difference(src,im); hist=diff.histogram()
    mse=sum((i%256)**2*n for i,n in enumerate(hist))/(src.width*src.height*3)
    psnr=10*math.log10(255**2/mse) if mse else float('inf')
    im.save(root/(stem+'.png'))
    rows.append([stem,src.width,src.height,colors,f.stat().st_size,round(psnr,2)])
with (root/'quality.csv').open('w',newline='',encoding='utf-8') as f:
    csv.writer(f).writerows([['fixture','width','height','colors','bytes','PSNR_dB'],*rows])
for name in ('gradient','local-wallpaper','japanese','\u65e5\u672c\u8a9e-\u7d30\u7dda'):
    if not (root/(name+'.png')).exists():continue
    src=Image.open(root/(name+'.ppm')).convert('RGB');target=src.size
    low=Image.open(root/(name+'.text.ppm')).convert('RGB').resize(target,Image.Resampling.NEAREST)
    high=Image.open(root/(name+'.png'))
    sheet=Image.new('RGB',(target[0]*3,target[1]+28),'#dddddd');d=ImageDraw.Draw(sheet)
    for i,(label,im) in enumerate([('WIC text raster',low),('WIC pixel raster',src),('Sixel decoded, 256',high)]):
        sheet.paste(im,(i*target[0],28));d.text((i*target[0]+4,5),label,fill='black')
    sheet.save(root/(name+'-compare.png'))
print(*rows,sep='\n')
