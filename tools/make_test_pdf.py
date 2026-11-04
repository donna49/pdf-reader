#!/usr/bin/env python3
"""Write a small multi-page PDF (text, outline, internal link) used by the CI smoke test."""
import sys

PAGES = 12


def build(out):
    objs = {}
    # 1 catalog, 2 pages, 3 font, 4 outlines, then per page: page, contents, outline item
    first = 5
    kids = []
    for i in range(PAGES):
        p, c, o = first + 3 * i, first + 3 * i + 1, first + 3 * i + 2
        kids.append(p)
        text = (f"BT /F1 28 Tf 72 720 Td (FeatherPDF test page {i + 1}) Tj ET\n"
                f"BT /F1 12 Tf 72 690 Td (The quick brown fox jumps over the lazy dog.) Tj ET\n"
                f"BT /F1 12 Tf 72 670 Td (Click here to jump to the last page.) Tj ET\n")
        objs[c] = f"<< /Length {len(text)} >>\nstream\n{text}endstream"
        link = f"<< /Type /Annot /Subtype /Link /Rect [72 665 300 682] /Border [0 0 0] /Dest [{first + 3 * (PAGES - 1)} 0 R /XYZ 0 792 0] >>"
        objs[p] = (f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents {c} 0 R "
                   f"/Resources << /Font << /F1 3 0 R >> >> /Annots [{link}] >>")
        prev = f"/Prev {o - 3} 0 R " if i > 0 else ""
        nxt = f"/Next {o + 3} 0 R " if i < PAGES - 1 else ""
        objs[o] = f"<< /Title (Page {i + 1}) /Parent 4 0 R {prev}{nxt}/Dest [{p} 0 R /XYZ 0 792 0] >>"
    objs[1] = "<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R /PageMode /UseOutlines >>"
    objs[2] = f"<< /Type /Pages /Kids [{' '.join(f'{k} 0 R' for k in kids)}] /Count {PAGES} >>"
    objs[3] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
    objs[4] = f"<< /Type /Outlines /First {first + 2} 0 R /Last {first + 3 * (PAGES - 1) + 2} 0 R /Count {PAGES} >>"

    data = b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"
    n = max(objs) + 1
    offsets = [0] * n
    for k in range(1, n):
        offsets[k] = len(data)
        data += f"{k} 0 obj\n{objs[k]}\nendobj\n".encode("latin-1")
    xref = len(data)
    data += f"xref\n0 {n}\n0000000000 65535 f \n".encode()
    for k in range(1, n):
        data += f"{offsets[k]:010d} 00000 n \n".encode()
    data += f"trailer\n<< /Size {n} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    with open(out, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "test.pdf")
