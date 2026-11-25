"""Generate local quality fixtures (Pillow is a developer-only dependency).
No sample image is linked into the executable. Run with an output directory.
"""
from pathlib import Path
import sys, math, shutil
from PIL import Image, ImageDraw, ImageFont
out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
w,h=2400,1600
im=Image.new("RGB",(w,h)); im.putdata([(x*255//(w-1), y*255//(h-1), (x+y)*255//(w+h-2)) for y in range(h) for x in range(w)])
im.save(out/"gradient.png")
im=Image.new("RGB",(w,h),"white"); d=ImageDraw.Draw(im)
for x in range(0,w,9): d.line((x,0,w-x,h), fill="black",width=1)
d.text((100,100),"細線と日本語文字 0123456789 E=mc²",font=ImageFont.truetype("C:/Windows/Fonts/meiryo.ttc",40),fill="red")
im.save(out/"日本語-細線.png")
im=Image.new("RGBA",(900,600));d=ImageDraw.Draw(im)
for i in range(250,0,-1): d.ellipse((450-i,300-i,450+i,300+i), fill=(40,160,255,255-i))
im.save(out/"alpha.png")
im=Image.new("RGB",(w,h));im.putdata([((255,255,255) if ((x//3+y//3)%2) else (0,0,0)) for y in range(h) for x in range(w)])
im.save(out/"checker.png")
im=Image.new("RGB",(w,h),"#102030");d=ImageDraw.Draw(im)
for i in range(300):
 x=(i*137)%w;y=(i*79)%h;d.ellipse((x,y,x+200,y+200),outline=(i%256,(i*3)%256,(i*7)%256),width=3)
im.save(out/"illustration.png")
im.resize((16,16)).save(out/"icon.png")
photo=Path("C:/Windows/Web/Wallpaper/Spotlight/img14.jpg")
if photo.exists(): shutil.copyfile(photo,out/"local-wallpaper.jpg")
(out/"sixel.ini").write_text("[preview]\nimages=true\ngraphics=auto\nimage_quality=high\n[meta]\nonboarding_completed=true\n",encoding="utf-8")
print(out)

# Vector table, fine type, formula text, chart and a rotated second page.
sys.path.insert(0, str(Path(__file__).parent / "proto"))
from make_pdfs import assemble, stream_obj
commands = ["BT /F1 8 Tf 30 760 Td (Thin type ABC123 E = mc^2, integral f[x] dx) Tj ET"]
for y in range(80, 720, 16): commands.append(f"0.2 w 30 {y} m 580 {y} l S")
for x in range(30, 590, 55): commands.append(f"0.2 w {x} 80 m {x} 720 l S")
for i in range(30): commands.append(f"BT /F1 7 Tf 35 {100+i*16} Td (Row {i}: 123.456 + 789 / sqrt[2]) Tj ET")
for i in range(120):
 x=40+i*4;y=400+int(80*math.sin(i/10))
 commands.append(f"0.3 0.5 0.9 RG 0.7 w {x} {y} m {x+4} {400+int(80*math.sin((i+1)/10))} l S")
content="\n".join(commands).encode("ascii")
objs={1:b"<< /Type /Catalog /Pages 2 0 R >>",2:b"<< /Type /Pages /Count 2 /Kids [5 0 R 6 0 R] >>",3:b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",4:stream_obj(content)}
for n,angle in [(5,0),(6,90)]:
 objs[n]=f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Rotate {angle} /Resources << /Font << /F1 3 0 R >> >> /Contents 4 0 R >>".encode()
(out/"high-detail.pdf").write_bytes(assemble(objs))
# A generated high-resolution raster page, for scanned-PDF checks.
import zlib
scan=Image.open(out/"日本語-細線.png").convert("RGB")
blob=zlib.compress(scan.tobytes())
objs={1:b"<< /Type /Catalog /Pages 2 0 R >>",2:b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",3:b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 400] /Resources << /XObject << /I 5 0 R >> >> /Contents 4 0 R >>",4:stream_obj(b"q 600 0 0 400 0 0 cm /I Do Q"),5:f"<< /Type /XObject /Subtype /Image /Width 2400 /Height 1600 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /Length {len(blob)} >>\nstream\n".encode()+blob+b"\nendstream"}
(out/"scanned.pdf").write_bytes(assemble(objs))
