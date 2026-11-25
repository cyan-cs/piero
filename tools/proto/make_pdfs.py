"""Writes small PDFs for the preview prototypes: no third-party library.

  python tools/proto/make_pdfs.py <out_dir>

two_page.pdf   page 1 normal, page 2 /Rotate 90, text in Helvetica
truncated.pdf  two_page.pdf cut in the middle of its xref data
garbage.pdf    a PDF header followed by noise
encrypted.pdf  RC4-40, user password "secret"
japanese.pdf   non-embedded MS-Gothic (Shift-JIS) text
"""
import hashlib
import os
import struct
import sys


def assemble(objs, trailer_extra=b""):
    """objs: dict number -> body bytes (1-based, consecutive)."""
    out = bytearray(b"%PDF-1.4\n")
    offsets = {}
    for n in sorted(objs):
        offsets[n] = len(out)
        out += b"%d 0 obj\n" % n + objs[n] + b"\nendobj\n"
    xref = len(out)
    count = len(objs) + 1
    out += b"xref\n0 %d\n0000000000 65535 f \n" % count
    for n in sorted(objs):
        out += b"%010d 00000 n \n" % offsets[n]
    out += (b"trailer\n<< /Size %d /Root 1 0 R %s >>\nstartxref\n%d\n%%%%EOF\n"
            % (count, trailer_extra, xref))
    return bytes(out)


def stream_obj(data):
    return b"<< /Length %d >>\nstream\n" % len(data) + data + b"\nendstream"


def pages_pdf(pages):
    """pages: list of (rotate, text)."""
    objs = {1: b"<< /Type /Catalog /Pages 2 0 R >>",
            3: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"}
    kids = []
    n = 4
    for rotate, text in pages:
        content = ("BT /F1 36 Tf 60 700 Td (%s) Tj ET\n"
                   "0.9 0.2 0.2 rg 60 500 300 120 re f\n"
                   "0.2 0.4 0.9 rg 160 420 300 120 re f\n" % text).encode()
        objs[n] = stream_obj(content)
        objs[n + 1] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                       b"/Rotate %d /Resources << /Font << /F1 3 0 R >> >> "
                       b"/Contents %d 0 R >>" % (rotate, n))
        kids.append(n + 1)
        n += 2
    objs[2] = b"<< /Type /Pages /Count %d /Kids [%s] >>" % (
        len(kids), b" ".join(b"%d 0 R" % k for k in kids))
    return assemble(objs)


PAD = bytes.fromhex(
    "28BF4E5E4E758A4164004E56FFFA01082E2E00B6D0683E802F0CA9FE6453697A")


def rc4(key, data):
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 255
        s[i], s[j] = s[j], s[i]
    out = bytearray()
    i = j = 0
    for b in data:
        i = (i + 1) & 255
        j = (j + s[i]) & 255
        s[i], s[j] = s[j], s[i]
        out.append(b ^ s[(s[i] + s[j]) & 255])
    return bytes(out)


def encrypted_pdf(user_pw=b"secret"):
    """One page, standard security handler R=2 (RC4, 40 bit), user password."""
    owner_key = hashlib.md5((b"owner" + PAD)[:32]).digest()[:5]
    o = rc4(owner_key, (user_pw + PAD)[:32])
    p = -4
    fid = bytes(range(16))
    key = hashlib.md5((user_pw + PAD)[:32] + o + struct.pack("<i", p) +
                      fid).digest()[:5]
    u = rc4(key, PAD)

    def object_key(num):
        return hashlib.md5(key + struct.pack("<I", num)[:3] +
                           bytes(2)).digest()[:10]

    content = rc4(object_key(4), b"BT /F1 36 Tf 60 700 Td (Secret page) Tj ET\n")
    objs = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
           b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        4: stream_obj(content),
        5: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        6: b"<< /Filter /Standard /V 1 /R 2 /O <%s> /U <%s> /P %d >>" % (
            o.hex().encode(), u.hex().encode(), p),
    }
    return assemble(objs, b"/Encrypt 6 0 R /ID [<%s> <%s>]" % (
        fid.hex().encode(), fid.hex().encode()))


def japanese_pdf():
    """Non-embedded MS-Gothic, Shift-JIS text: relies on the system font."""
    text = "日本語のPDF 漢字かな交じり文".encode("cp932")
    content = b"BT /F1 32 Tf 60 700 Td <" + text.hex().encode() + b"> Tj ET\n"
    objs = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: b"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        3: b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
           b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        4: stream_obj(content),
        5: b"<< /Type /Font /Subtype /Type0 /BaseFont /MS-Gothic "
           b"/Encoding /90ms-RKSJ-H /DescendantFonts [6 0 R] >>",
        6: b"<< /Type /Font /Subtype /CIDFontType2 /BaseFont /MS-Gothic "
           b"/CIDSystemInfo << /Registry (Adobe) /Ordering (Japan1) "
           b"/Supplement 2 >> /FontDescriptor 7 0 R /DW 1000 >>",
        7: b"<< /Type /FontDescriptor /FontName /MS-Gothic /Flags 5 "
           b"/FontBBox [0 -137 1000 859] /ItalicAngle 0 /Ascent 859 "
           b"/Descent -137 /CapHeight 700 /StemV 80 >>",
    }
    return assemble(objs)


def main():
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    good = pages_pdf([(0, "Page one"), (90, "Page two rotated")])

    def put(name, data):
        with open(os.path.join(d, name), "wb") as f:
            f.write(data)

    samples = {
        "two_page": good,
        "truncated": good[:len(good) - 120],
        "encrypted": encrypted_pdf(),
        "japanese": japanese_pdf(),
        "garbage": b"%PDF-1.7\n" + bytes((i * 37 + 11) & 255 for i in range(4000)),
    }
    for name, data in samples.items():
        put(name + ".pdf", data)
    if len(sys.argv) > 2:
        # a C header for the unit tests: python make_pdfs.py <dir> <header.h>
        with open(sys.argv[2], "w", newline="\n") as h:
            h.write("/* Generated by tools/proto/make_pdfs.py: do not edit. */\n")
            h.write("#ifndef WF_PDF_SAMPLES_H\n#define WF_PDF_SAMPLES_H\n\n")
            h.write("/* clang-format off */\n")
            for name, data in samples.items():
                h.write("static const unsigned char pdf_%s[%d] = {\n" %
                        (name, len(data)))
                for i in range(0, len(data), 16):
                    h.write("\t" + ", ".join("0x%02X" % b
                                             for b in data[i:i + 16]) + ",\n")
                h.write("};\n\n")
            h.write("/* clang-format on */\n\n#endif\n")


main()
