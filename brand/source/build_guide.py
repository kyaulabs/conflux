#!/usr/bin/env python3
"""Build the CONFLUX brand manual from the agreed brand specification.

Run with the bundled Python runtime. --draft uses placeholders only for authoring
and writes to work/; final generation requires every referenced master and font.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from xml.sax.saxutils import escape

from PIL import Image
from reportlab.lib.colors import HexColor, Color
from reportlab.lib.enums import TA_LEFT, TA_CENTER
from reportlab.lib.styles import ParagraphStyle
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas
from reportlab.lib.utils import ImageReader
from reportlab.platypus import Paragraph

HERE = Path(__file__).resolve().parent
if (HERE / "brand-spec.json").exists():
    SPEC_PATH = HERE / "brand-spec.json"
    SPEC = json.loads(SPEC_PATH.read_text())
    ROOT = Path(SPEC["root"]).resolve()
elif (HERE.parent / "tokens/brand-tokens.json").exists():
    ROOT = HERE.parent
    SPEC_PATH = ROOT / "tokens/brand-tokens.json"
    SPEC = json.loads(SPEC_PATH.read_text())
else:
    raise FileNotFoundError("Place this script in the brand pack's source/ directory, beside ../tokens/brand-tokens.json")
C = SPEC["colors"]
C.setdefault("darkControlBorder", "#78808A")
W, H, M = 960, 720, 56
GAP = 24
CW = (W - 2 * M - GAP) / 2
parser = argparse.ArgumentParser()
parser.add_argument("--draft", action="store_true")
ARGS = parser.parse_args()
OUT = HERE / "draft-guide.pdf" if ARGS.draft else ROOT / "CONFLUX-brand-guide.pdf"
OUT.parent.mkdir(parents=True, exist_ok=True)


def col(name):
    return HexColor(C.get(name, name))


FONTS = {
    "UI": "Barlow-Regular.ttf",
    "Medium": "Barlow-Medium.ttf",
    "Bold": "Barlow-SemiBold.ttf",
    "Display": "BarlowCondensed-SemiBold.ttf",
    "Mono": "JetBrainsMono-Regular.ttf",
}
for name, fname in FONTS.items():
    p = ROOT / "assets/fonts" / fname
    if p.exists():
        pdfmetrics.registerFont(TTFont(name, str(p)))
    elif ARGS.draft:
        pdfmetrics.registerFont(pdfmetrics.Font(name, "Courier" if name == "Mono" else "Helvetica-Bold" if name in ("Display", "Bold") else "Helvetica", "WinAnsiEncoding"))
    else:
        raise FileNotFoundError(p)
pdfmetrics.registerFontFamily("UI", normal="UI", bold="Bold", italic="UI", boldItalic="Bold")

cv = canvas.Canvas(str(OUT), pagesize=(W, H), pageCompression=1)
cv.setTitle("CONFLUX - Brand Guide / Version 1.1")
cv.setAuthor("CONFLUX")
cv.setSubject("Visual identity, digital themes, product expression and Linux asset handoff")
cv.setKeywords("CONFLUX, brand, DJ, Linux, KDE, Qt, visual identity")
CURRENT_PAGE = 0
CURRENT_DARK = False
TEXT_BOXES = []


def clean(s):
    return str(s).replace("\u2014", " - ").replace("\u2013", "-").replace("\u2011", "-")


def ptext(s, x, top, width, size=14, leading=None, color=None, font="UI", max_h=None, align=TA_LEFT):
    leading = leading or size * 1.38
    color = color or ("darkText" if CURRENT_DARK else "charcoal")
    style = ParagraphStyle("p", fontName=font, fontSize=size, leading=leading,
                           textColor=col(color), alignment=align, spaceAfter=0,
                           allowWidows=0, allowOrphans=0, borderPadding=0)
    para = Paragraph(clean(s), style)
    _, ht = para.wrap(width, H)
    if max_h is not None and ht > max_h + .1:
        raise ValueError(f"Page {CURRENT_PAGE}: paragraph exceeds box by {ht-max_h:.1f}: {s[:80]}")
    if top > 45 and top - ht < 49:
        raise ValueError(f"Page {CURRENT_PAGE}: text runs into footer: {s[:80]}")
    para.drawOn(cv, x, top-ht)
    TEXT_BOXES.append((CURRENT_PAGE, x, top-ht, width, ht, clean(s)))
    return ht


def line(x1, y1, x2, y2, color="charcoal", width=1, dash=None):
    cv.saveState(); cv.setStrokeColor(col(color)); cv.setLineWidth(width)
    if dash: cv.setDash(*dash)
    cv.line(x1, y1, x2, y2); cv.restoreState()


def box(x, top, width, height, fill, radius=0, stroke=None, sw=1):
    cv.saveState(); cv.setFillColor(col(fill))
    if stroke: cv.setStrokeColor(col(stroke)); cv.setLineWidth(sw)
    cv.roundRect(x, top-height, width, height, radius, fill=1, stroke=bool(stroke))
    cv.restoreState()


def label(s, x, top, color=None, size=10, width=700):
    return ptext(escape(clean(s)), x, top, width, size, leading=size*1.1, color=color, font="Bold")


def rule(top, x=M, width=W-2*M, color=None):
    line(x, top, x+width, top, color or ("darkBorder" if CURRENT_DARK else "#D9D9D4"), .65)


def logo(name, x, top, width, height, opacity=1):
    path = ROOT / "assets/logos" / (name + ".png")
    if not path.exists():
        if not ARGS.draft: raise FileNotFoundError(path)
        box(x, top, width, height, "darkRaised" if CURRENT_DARK else "lightRaised")
        ptext(name.replace("conflux-", ""), x+8, top-7, width-16, 8, 10, max_h=max(12,height-14))
        return (x, top-height, width, height)
    im = Image.open(path).convert("RGBA")
    bounds = im.getchannel("A").getbbox()
    if bounds: im = im.crop(bounds)
    ratio = min(width/im.width, height/im.height)
    iw, ih = im.width*ratio, im.height*ratio
    ix, iy = x+(width-iw)/2, top-(height+ih)/2
    cv.saveState()
    if opacity != 1: cv.setFillAlpha(opacity)
    cv.drawImage(ImageReader(im), ix, iy, iw, ih, mask="auto")
    cv.restoreState()
    return (ix, iy, iw, ih)


def asset_image(rel, x, top, width, height):
    path = ROOT / rel
    if not path.exists():
        if not ARGS.draft: raise FileNotFoundError(path)
        box(x, top, width, height, "darkRaised" if CURRENT_DARK else "lightRaised", 16)
        return
    im = Image.open(path)
    scale=min(width/im.width,height/im.height)
    iw,ih=im.width*scale,im.height*scale
    cv.drawImage(ImageReader(im),x+(width-iw)/2,top-(height+ih)/2,iw,ih,mask="auto")


def horizontal_nucleus_geometry(drawn_bounds):
    """Map the flat-to-flat core height to its scaled horizontal placement.

    The approved flat-top rounded hexagon uses R=101 and corner setback=18.
    Rounding leaves the upper/lower flats at y=250 +/- 101*sqrt(3)/2.
    Therefore x = 101*sqrt(3), not the core's point-to-point width.
    """
    bx,by,bw,bh=drawn_bounds
    path=ROOT/"assets/logos/conflux-horizontal-light.png"
    if not path.exists(): return (bx+42,by+bh/2,30)
    im=Image.open(path).convert("RGBA")
    left,top,right,bottom=im.getchannel("A").getbbox()
    sx,sy=im.width/1200,im.height/360
    nx=bx+((8+256*.66)*sx-left)*bw/(right-left)
    ny=by+bh-((12+250*.66)*sy-top)*bh/(bottom-top)
    flat_height=101*math.sqrt(3)*.66*sy*bh/(bottom-top)
    return nx,ny,flat_height


def page(number, section, title=None, subtitle=None, dark=False):
    global CURRENT_PAGE, CURRENT_DARK
    CURRENT_PAGE, CURRENT_DARK = number, dark
    cv.setFillColor(col("darkCanvas" if dark else "ivory")); cv.rect(0, 0, W, H, fill=1, stroke=0)
    label(f"{number:02d} / {section.upper()}", M, H-38, "darkMuted" if dark else "lightMuted", 10)
    if title: ptext(title, M, H-76, 830, 45, 46, font="Display", max_h=100)
    if subtitle: ptext(subtitle, M, H-136, 820, 14, 19, color="darkMuted" if dark else "lightMuted", max_h=60)
    line(M, 45, W-M, 45, "darkBorder" if dark else "#D9D9D4", .6)
    label("CONFLUX / BRAND SYSTEM", M, 30, "darkMuted" if dark else "lightMuted", 8)
    ptext("VERSION 1.1 / 15 OCT 2026", W/2-110, 30, 220, 8, 10, color="darkMuted" if dark else "lightMuted", align=TA_CENTER)
    ptext(f"{number:02d} / 16", W-M-60, 30, 60, 8, 10, color="darkMuted" if dark else "lightMuted", font="Mono", align=2)


def end(): cv.showPage()


def mini_header(kicker, title, body, x, top, width=CW, dark=False):
    label(kicker, x, top, "darkAccent" if dark else "lightAccent", 10, width)
    ptext(title, x, top-25, width, 25, 28, font="Display", max_h=60)
    ptext(body, x, top-96, width, 13, 18, color="darkMuted" if dark else "lightMuted", max_h=130)


def pill(txt, x, top, width, fill, textcolor, border=None):
    box(x, top, width, 31, fill, 6, border)
    ptext(txt, x+8, top-8, width-16, 10, 12, textcolor, "Bold", max_h=15, align=TA_CENTER)


def luminance(h):
    rgb=[int(h[i:i+2],16)/255 for i in (1,3,5)]
    vals=[v/12.92 if v <= .04045 else ((v+.055)/1.055)**2.4 for v in rgb]
    return sum(a*b for a,b in zip(vals,[.2126,.7152,.0722]))


def contrast(a,b):
    a,b=luminance(C.get(a,a)),luminance(C.get(b,b))
    return (max(a,b)+.05)/(min(a,b)+.05)


def swatch(name, token, x, top, width, height=102, on=None):
    on=on or ("charcoal" if luminance(C[token])>.45 else "white")
    box(x,top,width,height,token)
    label(name.upper(),x+16,top-17,on,11,width-32)
    ptext(C[token],x+16,top-height+30,width-32,12,15,on,"Mono",max_h=20)


def wave(x, mid, width, height, color, phase=0, bars=110):
    cv.saveState();cv.setStrokeColor(col(color));cv.setLineWidth(1.45)
    for i in range(bars):
        a=abs(math.sin(i*.31+phase)*math.cos(i*.117+phase)+.18*math.sin(i*.91))
        bh=4+a*(height/2-4)
        bx=x+i*(width/(bars-1));cv.line(bx,mid-bh,bx,mid+bh)
    cv.restoreState()


def play_icon(cx, cy, size, color):
    cv.saveState();cv.setFillColor(col(color));pt=cv.beginPath()
    pt.moveTo(cx-size*.32,cy-size*.45);pt.lineTo(cx-size*.32,cy+size*.45);pt.lineTo(cx+size*.48,cy);pt.close()
    cv.drawPath(pt,fill=1,stroke=0);cv.restoreState()


# 01 - Cover
page(1, "Identity", dark=True)
label("MUSIC, DIRECTED.", M, 598, "darkAccent", 12)
ptext("Brand<br/>system.", M, 552, 440, 94, 88, font="Display", max_h=200)
ptext("Two currents. One performance.", M+3, 329, 420, 22, 28, color="darkMuted")
logo("conflux-symbol-dark", 520, 565, 350, 350)
logo("conflux-wordmark-dark", M, 185, 400, 70)
label("THE CONFLUX VISUAL IDENTITY", 586, 147, "darkMuted", 10, 300)
ptext("Version 1.1<br/>15 October 2026", 586, 123, 300, 12, 17, color="darkMuted")
end()

# 02 - Idea
page(2,"Brand idea","Human expression. Machine direction.","CONFLUX helps a performer arrange the set while keeping the expressive mix in their hands.")
ptext("CONFLUX is an open-source Linux DJ application concept: AI helps choose tracks and coordinate transitions; the performer shapes the sound with EQ and effects.",M,526,382,19,26,max_h=120)
ptext("The name describes currents meeting. Two matching orbital currents turn 180 degrees around a centered, rounded hexagonal nucleus: movement with balance, energy with control.",M,389,382,14,20,color="lightMuted",max_h=100)
cards=[("01","PERFORMANCE FIRST","Decisions should make space for listening, timing and expressive control."),
       ("02","CALM UNDER PRESSURE","Clear hierarchy, stable placement and explicit status belong on every screen."),
       ("03","OPEN BY DESIGN","A Linux-native identity with useful assets and transparent, inspectable behavior."),
       ("04","ENERGY WITH INTENT","Use red to direct attention; let rhythm and proportion create the atmosphere.")]
for i,(n,t,b) in enumerate(cards):
    x=514+(i%2)*200;top=526-(i//2)*166
    label(n,x,top,"lightAccent",11,170)
    ptext(t,x,top-26,174,18,20,font="Display",max_h=45)
    ptext(b,x,top-79,174,12,17,color="lightMuted",max_h=80)
rule(178)
label("TWO CLEAR OWNERS",M,158,"lightAccent")
ptext("<b>DIRECTOR</b> / Track selection, cues, transport, timing and crossfade.",M,133,400,12,17,max_h=48)
ptext("<b>LIVE MIX</b> / Performer-owned EQ and effects. Automation never silently takes over these controls.",514,133,390,12,17,max_h=55)
end()

# 03 - Logo anatomy
page(3,"Logo anatomy","A recognizable meeting of currents.","The approved symmetric rounded-hex mark is the master. Use the supplied outlined artwork; never rebuild it from type.")
box(M,530,350,314,"white",10)
logo("conflux-symbol-light",M+55,496,240,240)
label("THE SYMBOL",M+20,230,"lightAccent",10,300)
ptext("Equal currents, rotated 180 degrees. A centered flat-top hexagon with softened corners. R101 / corner setback 18.",M+20,208,308,12,17,max_h=55)
logo("conflux-wordmark-light",458,521,422,90)
label("CUSTOM WORDMARK",458,409,"lightAccent",10,400)
ptext("Tall, condensed forms carry the name. The red diamond inside the X connects the wordmark to the symbol's directional energy. Letter shapes and spacing are fixed artwork.",458,385,420,13,18,max_h=90)
rule(282,x=458,width=422)
label("APPROVED COMPOSITIONS",458,263,"lightMuted",10)
logo("conflux-horizontal-light",458,232,232,70)
logo("conflux-stacked-light",730,253,146,117)
label("HORIZONTAL",458,145,"lightMuted",9,220)
label("STACKED",730,117,"lightMuted",9,146)
label("MASTERS",M,113,"lightMuted",9)
ptext("SVG for scaling and export. PNG for raster placement. Light and dark variants are intentionally different. Use one-color black or white masters when color cannot reproduce.",M,94,600,11,15,max_h=35)
end()

# 04 - Geometry
page(4,"Logo rules","Give the identity room to breathe.","Clear space protects the silhouette. Minimum sizes protect the details.")
box(M,528,405,276,"white",10)
bx,by,bw,bh=logo("conflux-horizontal-light",M+60,442,285,80)
nx,ny,xunit=horizontal_nucleus_geometry((bx,by,bw,bh))
cv.saveState();cv.setStrokeColor(col("lightAccent"));cv.setLineWidth(.7);cv.setDash(3,3)
cv.rect(bx-.5*xunit,by-.5*xunit,bw+xunit,bh+xunit,stroke=1,fill=0)
cv.restoreState()
line(bx-.5*xunit,by+bh+24,bx,by+bh+24,"lightAccent",1)
ptext("0.5x",bx-10,by+bh+45,55,10,12,"lightAccent","Mono")
xrule=bx-33
line(xrule,ny-xunit/2,xrule,ny+xunit/2,"lightAccent",.8)
line(xrule-3,ny-xunit/2,xrule+3,ny-xunit/2,"lightAccent",.8)
line(xrule-3,ny+xunit/2,xrule+3,ny+xunit/2,"lightAccent",.8)
line(xrule+4,ny-xunit/2,nx,ny-xunit/2,"lightAccent",.45,dash=(1,3))
line(xrule+4,ny+xunit/2,nx,ny+xunit/2,"lightAccent",.45,dash=(1,3))
ptext("x",xrule-13,ny+5,20,10,12,"lightAccent","Mono")
label("x = CORE FLAT-TO-FLAT HEIGHT",M+20,314,"lightAccent",10,360)
ptext("x is the hexagon's flat-to-flat height (174.94 master units). Keep at least 0.5x clear around every logo's outer visible bounds.",M+20,291,365,12,17,max_h=50)
label("MINIMUM REPRODUCTION",510,525,"lightAccent")
sizes=[("Horizontal lockup","180 px / 40 mm"),("Stacked lockup","140 px / 32 mm"),("Wordmark","120 px / 28 mm"),("Standard symbol","32 px"),("Simplified symbolic mark","16-24 px")]
for i,(a,b) in enumerate(sizes):
    t=494-i*38
    ptext(a,510,t,230,12,16,max_h=20)
    ptext(b,741,t,160,11,16,font="Mono",max_h=20)
    rule(t-25,x=510,width=391)
ptext("Pixel values are nominal screen sizes. Use the supplied small-size assets and inspect the final raster at 100%. Print values assume a clean production process.",510,281,390,11,16,color="lightMuted",max_h=60)
label("NEVER",M,203,"lightAccent")
misuses=[("Distort","Do not stretch, skew or change stroke proportions."),("Recompose","Do not move the nucleus, change the red diamond or retype the name."),("Decorate","No shadows, gradients, glow, outlines or perpetual rotation.")]
for i,(a,b) in enumerate(misuses):
    xx=M+i*289
    ptext(a,xx,175,265,23,26,font="Display",max_h=35)
    ptext(b,xx,138,261,12,17,color="lightMuted",max_h=65)
end()

# 05 - Palette
page(5,"Color palette","Signal, structure and breathing room.","The signature red carries energy. Charcoal and ivory provide the stable field around it.")
swatch("Signal","signal",M,527,280,145,"charcoal")
swatch("Charcoal","charcoal",336,527,284,145,"white")
swatch("Ivory","ivory",620,527,284,145,"charcoal")
ptext("Use signature red for the brand mark, high-level emphasis and selected visual accents. It is not the default color for every interactive element or every line of text.",M,355,400,13,18,max_h=90)
ptext("Build interfaces from semantic theme roles. Theme-specific accent, primary and deck colors keep the identity usable in both bright rooms and dark performance spaces.",508,355,396,13,18,max_h=90)
label("SEMANTIC SUPPORT / LIGHT AND DARK PAIRS",M,262,"lightAccent")
support=[("Deck A","deckALight","deckADark"),("Deck B","deckBLight","deckBDark"),("Success","successLight","successDark"),("Warning","warningLight","warningDark")]
for i,(name,a,b) in enumerate(support):
    xx=M+i*216
    box(xx,230,96,59,a,4);box(xx+100,230,96,59,b,4)
    label(name.upper(),xx,157,"lightMuted",10,200)
    ptext(C[a]+" / "+C[b],xx,136,202,9,13,font="Mono",max_h=30)
ptext("A color is a role, not the only cue. Deck labels, icons, focus outlines and text must carry the same information.",M,84,848,11,15,color="lightMuted",max_h=20)
end()

# 06 - Theme recipes
page(6,"Theme recipes","One identity. Two working environments.","Light and dark themes share hierarchy and behavior; each has tuned contrast and state colors.")
theme_rows=[("Canvas","Canvas"),("Surface","Surface"),("Raised","Raised"),("Text","Text"),("Muted","Muted"),("Accent","Accent"),("Divider","Border"),("Primary","Primary")]
for j,(mode,title) in enumerate([("light","LIGHT / DAY SESSION"),("dark","DARK / PERFORMANCE")]):
    xx=M+j*(CW+24);dark=mode=="dark"
    box(xx,529,CW,416,mode+"Surface",10)
    label(title,xx+20,509,"darkMuted" if dark else "lightMuted",10,CW-40)
    for i,(human,suffix) in enumerate(theme_rows):
        yy=476-i*37;token=mode+suffix
        box(xx+20,yy,27,23,token,3,"darkBorder" if dark else "#D9D9D4",.5)
        ptext(human,xx+60,yy-3,140,12,16,"darkText" if dark else "lightText",max_h=20)
        ptext(C[token],xx+237,yy-3,135,11,15,"darkMuted" if dark else "lightMuted","Mono",max_h=20)
    pill("PRIMARY ACTION",xx+20,158,158,mode+"Primary","darkCanvas" if dark else "white")
    ptext("Aa / 123.00",xx+211,152,175,16,20,mode+"Text","Mono",max_h=25)
ptext("Honor the system theme initially, then remember the performer's explicit setting. Dark control boundaries use #78808A; #505660 is for decorative dividers. Use darkAccent for keyboard focus.",M,92,848,12,17,color="lightMuted",max_h=35)
end()

# 07 - Accessibility
page(7,"Accessibility","Contrast is a specification, not a guess.","Ratios below are computed from the actual sRGB tokens using the WCAG relative-luminance method.")
rows=[("LIGHT", "Body text / surface","lightText","lightSurface","Text AA"),
      ("LIGHT", "Muted text / surface","lightMuted","lightSurface","Text AA"),
      ("LIGHT", "Accent text / surface","lightAccent","lightSurface","Text AA"),
      ("LIGHT", "White / primary button","white","lightPrimary","Text AA"),
      ("LIGHT", "Essential border / surface","lightBorder","lightSurface","UI 3:1"),
      ("LIGHT", "Deck A / surface","deckALight","lightSurface","Text AA"),
      ("LIGHT", "Deck B / surface","deckBLight","lightSurface","Text AA"),
      ("DARK", "Body text / surface","darkText","darkSurface","Text AA"),
      ("DARK", "Muted text / surface","darkMuted","darkSurface","Text AA"),
      ("DARK", "Accent text / surface","darkAccent","darkSurface","Text AA"),
      ("DARK", "Canvas ink / primary button","darkCanvas","darkPrimary","Text AA"),
      ("DARK", "Essential border / surface","darkControlBorder","darkSurface","UI 3:1"),
      ("DARK", "Deck A / surface","deckADark","darkSurface","Text AA"),
      ("DARK", "Deck B / surface","deckBDark","darkSurface","Text AA")]
label("THEME",M,531,"lightMuted",9,70)
label("FOREGROUND / BACKGROUND",130,531,"lightMuted",9,430)
label("RATIO",680,531,"lightMuted",9,80)
label("TARGET",793,531,"lightMuted",9,100)
rule(512)
for i,(mode,name,a,b,target) in enumerate(rows):
    yy=499-i*23
    box(M,yy,50,17,b,3)
    ptext(mode,M+5,yy-4,44,7,9,a,"Bold",max_h=10)
    ptext(name,130,yy-1,475,11,14,max_h=18)
    ptext(f"{contrast(a,b):.2f}:1",680,yy-1,90,11,14,font="Mono",max_h=18)
    ptext(target,793,yy-1,100,10,14,color="successLight",font="Bold",max_h=18)
    rule(yy-19)
label("TWO IMPORTANT EXCEPTIONS",M,155,"lightAccent",10)
ptext(f"<b>Signal on ivory: {contrast('signal','ivory'):.2f}:1.</b> Suitable for large text and brand graphics; use lightAccent for normal-size links and labels.",M,133,405,11,15,max_h=48)
ptext(f"<b>Dark divider on surface: {contrast('darkBorder','darkSurface'):.2f}:1.</b> Decorative only. Essential boundaries use darkControlBorder (#78808A); focus uses darkAccent.",500,133,404,11,15,max_h=48)
ptext("AA text: 4.5:1. Large text and essential UI graphics: 3:1. Rounded ratios are for display; pass/fail uses unrounded values. Contrast alone is not a full accessibility audit.",M,77,848,9,12,color="lightMuted",max_h=26)
end()

# 08 - Typography
page(8,"Typography","Condensed energy. Unhurried reading.","The logo is custom artwork. The supporting type system is open and practical.")
label("DISPLAY / BARLOW CONDENSED SEMIBOLD",M,530,"lightAccent",10)
ptext("SHAPE THE<br/>MOMENT.",M,492,450,72,68,font="Display",max_h=150)
label("BODY AND INTERFACE / BARLOW",M,309,"lightAccent",10)
ptext("Keep the music in motion.",M,279,430,28,34,font="UI",max_h=45)
ptext("Clear labels, readable metadata and predictable spacing make a complex performance surface feel calm. Use sentence case for body copy and controls.",M,231,402,14,21,max_h=110)
label("DATA / JETBRAINS MONO",M,115,"lightAccent",10)
ptext("128.00 BPM / 08:32 / -03.5 dB",M,90,450,15,20,font="Mono",max_h=25)
label("DIGITAL SCALE",540,530,"lightMuted",10,360)
scale=[("Hero",64,68,"Display"),("Page title",40,44,"Display"),("Section title",28,32,"Display"),("Body / UI",16,24,"UI"),("Secondary",14,20,"UI"),("Label",12,16,"Bold"),("Technical data",14,20,"Mono")]
yy=497
for name,sz,lh,font in scale:
    ptext(name,540,yy,214,min(sz,32),min(sz,32)+5,font=font,max_h=45)
    ptext(f"{sz}/{lh}",800,yy-2,100,11,15,font="Mono",max_h=20)
    rule(yy-43,x=540,width=364);yy-=55
ptext("Values are CSS px / line-height for digital implementation. This PDF uses a separate editorial scale. Avoid condensed text for dense metadata or long paragraphs.",540,101,364,11,15,color="lightMuted",max_h=45)
end()

# 09 - App icon
page(9,"Application icons","The symbol carries the app.","Launcher icons are a focused expression of the identity, not a miniature logo composition.")
asset_image("assets/app-icons/conflux-app-dark-512.png",M,528,272,272)
asset_image("assets/app-icons/conflux-app-light-512.png",360,528,272,272)
label("DARK TILE",M,235,"lightMuted",10,260)
label("LIGHT TILE",360,235,"lightMuted",10,260)
ptext("The symbol occupies roughly 76% of the square tile. Use the supplied launcher tile without adding another frame. For surfaces that apply their own mask, use the full-bleed maskable asset and preserve its safe area.",M,206,575,13,18,max_h=88)
label("PROVIDED SIZES",684,526,"lightAccent",10,210)
ptext("16 / 22 / 24 / 32<br/>48 / 64 / 128<br/>256 / 512 / 1024",684,497,220,15,25,font="Mono",max_h=95)
label("SMALL-SIZE DISCIPLINE",684,375,"lightAccent",10,210)
ptext("Use the simplified symbolic mark at 16-24 px. At 32 px and above, use the standard symbol. Avoid the wordmark, fine rules and soft shadows in launcher icons.",684,347,214,13,18,max_h=110)
rule(144)
for i,sz in enumerate([16,22,24,32,48,64]):
    xx=M+i*132
    asset_image(f"assets/app-icons/conflux-app-dark-{sz}.png",xx,122,sz,sz)
    ptext(str(sz)+" px",xx+sz+10,112,75,10,13,color="lightMuted",font="Mono",max_h=18)
end()

# 10 - Web identity
page(10,"Web and social","Recognizable before the page loads.","Use one coherent mark across the browser tab, installed web shortcut and project communications.")
box(M,528,520,262,"white",10)
asset_image("assets/social/conflux-open-graph-dark.png",M+16,515,488,224)
ptext("Included social preview / 1200 x 630 px",M+22,286,450,10,14,color="lightMuted",max_h=20)
label("WEB FILES",628,526,"lightAccent",10,270)
files=[("favicon.svg","Scalable browser icon"),("favicon.ico","Legacy 16/32/48 px fallback"),("apple-touch-icon.png","180 px touch shortcut"),("android-chrome-192x192.png","192 px install icon"),("android-chrome-512x512.png","512 px install icon"),("site.webmanifest","Linked install metadata")]
yy=496
for a,b in files:
    ptext(a,628,yy,280,9,13,font="Mono",max_h=20)
    ptext(b,628,yy-19,275,11,15,color="lightMuted",max_h=20);yy-=49
label("SOCIAL AVATAR",M,226,"lightAccent",10)
ptext("Use the symbol alone on charcoal or ivory. Keep it centered in the safe area and check a circular crop. The full name belongs in the profile name, not inside a tiny avatar.",M,200,404,13,18,max_h=100)
label("SOCIAL COVER",M+452,226,"lightAccent",10)
ptext("Use the horizontal or stacked master with a short message. Respect 0.5x clear space, keep text away from crop edges, and preview the actual service's mobile framing.",M+452,200,396,13,18,max_h=100)
ptext("Website and social placements shown in this guide are design concepts. No account, domain, product availability or launch status is implied.",M,86,848,11,15,color="lightMuted",max_h=30)
end()

# 11 - Components
page(11,"Components and motion","Controls that stay readable in the moment.","Small, repeatable rules connect the brand to usable performance software.")
label("ACTION HIERARCHY",M,529,"lightAccent",10)
pill("START DIRECTOR",M,498,174,"lightPrimary","white")
pill("REVIEW NEXT",M+192,498,151,"white","lightText","lightBorder")
pill("CANCEL",M+361,498,112,"lightRaised","lightMuted")
ptext("One primary action per local task. Use text labels for consequential transport actions and pair state colors with explicit status.",M,444,473,13,18,max_h=65)
label("FOCUS / STATE",M,343,"lightAccent",10)
box(M,311,473,73,"white",8,"lightAccent",2)
label("NEXT TRACK",M+16,296,"lightMuted",9,260)
ptext("Northbound / Extended mix",M+16,275,435,15,20,max_h=27)
ptext("A visible 2 px focus ring with separation from the control edge. Keep keyboard order aligned with the visual reading order.",M,217,473,13,18,max_h=65)
label("LAYOUT RHYTHM",589,529,"lightAccent",10,310)
ptext("4 / 8 / 12 / 16 / 24 / 32",589,496,315,16,22,font="Mono",max_h=30)
ptext("Use a 4 px spacing base. Prefer 8 px within controls, 16 px within groups and 24-32 px between task areas. Keep dense data aligned to a stable grid.",589,455,309,13,18,max_h=100)
label("MOTION",589,334,"lightAccent",10,310)
ptext("160 ms / micro interactions<br/>240 ms / panel transitions",589,304,314,14,23,font="Mono",max_h=55)
ptext("Animate state changes, not the identity itself. Respect reduced-motion settings; keep essential status understandable when animation is removed.",589,235,310,13,18,max_h=100)
rule(129)
ptext("Never rely on hover, sound or color alone. Use persistent labels, visible focus and clear stopped/armed/playing states. These rules are design requirements for implementation, not a claim of completed accessibility testing.",M,108,848,12,17,color="lightMuted",max_h=48)
end()


def deck_panel(x,top,width,label_txt,title,bpm,key,accent,phase):
    box(x,top,width,180,"darkSurface",7)
    label(label_txt,x+14,top-13,accent,9,width-28)
    ptext(title,x+14,top-36,width-28,18,22,"darkText","Display",max_h=28)
    ptext(f"{bpm} BPM / {key}",x+14,top-66,width-28,10,14,"darkMuted","Mono",max_h=18)
    wave(x+14,top-112,width-28,55,accent,phase,bars=75)
    line(x+width*.55,top-85,x+width*.55,top-141,"darkText",1)
    box(x+14,top-150,38,22,accent,4)
    play_icon(x+33,top-161,10,"darkCanvas")
    ptext("CUE",x+67,top-154,60,9,12,"darkMuted","Bold",max_h=15)


# 12 - Product concept
page(12,"DJ application concept","The director arranges. You shape the sound.","Concept interface / illustrative data / not a screenshot of a shipped application.",dark=True)
box(M,529,848,354,"darkSurface",9,"darkBorder",.6)
logo("conflux-horizontal-dark",M+15,516,170,31)
pill("DIRECTOR ARMED",M+640,515,190,"darkRaised","darkAccent")
deck_panel(M+15,463,300,"DECK A / PLAYING","Northbound", "128.00", "8A", "deckADark",.2)
deck_panel(M+533,463,300,"DECK B / CUED","Glass Current", "128.00", "9A", "deckBDark",1.2)
box(M+330,463,188,180,"darkRaised",7)
label("LIVE MIX / YOU",M+343,450,"darkText",9,161)
for i,n in enumerate(["HIGH","MID","LOW"]):
    cy=399-i*40
    for j in range(2):
        cx=M+365+j*94
        cv.setStrokeColor(col("darkMuted"));cv.setLineWidth(1);cv.circle(cx,cy,11,fill=0,stroke=1)
        line(cx,cy,cx+6,cy+7,"darkText",1.5)
    ptext(n,M+392,cy+4,56,8,10,"darkMuted","Bold",max_h=12,align=TA_CENTER)
label("EQ + EFFECTS",M+350,301,"darkMuted",9,150)
rule(267,x=M+15,width=818,color="darkBorder")
label("UP NEXT",M+16,252,"darkMuted",9,100)
ptext("Glass Current / 32-bar blend / next phrase",M+16,230,535,13,18,"darkText",max_h=24)
ptext("LOCKED 00:24",M+660,230,167,10,15,"darkAccent","Mono",max_h=20)
label("DESIGN INTENT",M,147,"darkAccent",10)
ptext("DIRECTOR owns selection, cues, transport, timing and crossfade. Keep its plan visible before execution, with a clear hold or takeover control.",M,123,404,12,17,"darkMuted",max_h=60)
ptext("LIVE MIX keeps EQ and effects under the performer's hands. Theme-specific deck colors identify sources without competing with the red brand signal.",500,123,404,12,17,"darkMuted",max_h=60)
end()

# 13 - Website
page(13,"Website expression","A stage for the identity.","Concept layout / visual direction for the included brand website.")
box(M,530,848,328,"charcoal",10)
logo("conflux-horizontal-dark",M+26,507,177,35)
label("IDENTITY     SYSTEM     ASSETS",M+536,495,"darkMuted",9,280)
label("MUSIC, DIRECTED.",M+30,425,"darkAccent",10,400)
ptext("Two currents.<br/>One performance.",M+30,391,490,52,51,"darkText","Display",max_h=110)
ptext("A visual system for the open-source Linux DJ application concept.",M+33,271,460,12,18,"darkMuted",max_h=40)
logo("conflux-symbol-dark",M+581,433,204,204)
label("COMPOSITION",M,165,"lightAccent",10,250)
ptext("Give the logo a calm field. Pair one strong condensed headline with short, plain body copy. Alternate ivory editorial sections with deep charcoal moments.",M,138,399,13,18,max_h=75)
label("WEB IMPLEMENTATION",507,165,"lightAccent",10,380)
ptext("Use semantic HTML, real text and the CSS tokens. Honor reduced motion. Keep the downloads easy to locate and preserve visible keyboard focus in both themes.",507,138,397,13,18,max_h=75)
end()

# 14 - Voice
page(14,"Voice and language","Confident, specific and in time.","CONFLUX sounds like a capable collaborator beside the decks: direct, calm and musically aware.")
tone=[("DIRECT","Say what will happen and when. Use concrete musical actions."),
      ("TRANSPARENT","Make automation state and control ownership visible."),
      ("HUMAN","Respect the performer. Offer clarity without hype or judgment.")]
for i,(a,b) in enumerate(tone):
    xx=M+i*289
    label(a,xx,527,"lightAccent",11,265)
    ptext(b,xx,498,260,14,20,max_h=95)
label("USE",M,376,"successLight",10,400)
label("AVOID",507,376,"lightMuted",10,397)
examples=[("Next transition in 32 bars.","The AI is about to work its magic."),
          ("Hold this track.","Disengage the autonomous orchestration subsystem."),
          ("Director paused. You control the decks.","Something went wrong."),
          ("EQ and effects stay with you.","Fully automatic perfection.")]
for i,(a,b) in enumerate(examples):
    yy=344-i*54
    ptext(a,M,yy,400,15,21,max_h=43)
    ptext(b,507,yy,397,13,19,color="lightMuted",max_h=43)
    rule(yy-42)
rule(108)
ptext("Write the name as <b>CONFLUX</b>. Keep the tagline exactly <b>Music, directed.</b> Use sentence case for controls. Prefer musical units such as bars and beats when they help the performer act; expose technical detail only where useful.",M,90,848,12,17,color="lightMuted",max_h=38)
end()

# 15 - Linux handoff
page(15,"Linux and Qt integration","Native assets. Predictable behavior.","Use the same identity from KDE's launcher to the Qt interface and the project website.")
label("DESKTOP INTEGRATION",M,528,"lightAccent",10,400)
ptext("Install application icons into the hicolor theme at the provided sizes, using one stable application icon name. Include the scalable SVG in the corresponding scalable/apps location.",M,498,400,13,19,max_h=100)
box(M,380,400,97,"white",6)
ptext("share/icons/hicolor/48x48/apps/<br/>org.example.Conflux.png<br/><br/>Icon=org.example.Conflux",M+16,362,370,10,15,font="Mono",max_h=80)
ptext("The example ID is a placeholder. Replace it with an identifier under a domain the project controls before packaging. Match icon, desktop-entry and application metadata IDs.",M,260,400,12,18,color="lightMuted",max_h=90)
label("SMALL CONTEXTS",M,146,"lightAccent",10,400)
ptext("Use conflux-symbolic.svg for compact system surfaces when appropriate; allow the host theme to supply foreground color. Preserve the launcher tile for full-color application identity.",M,120,400,12,17,max_h=60)
label("QT THEME CONNECTION",507,528,"lightAccent",10,397)
ptext("Treat brand-tokens.json as the portable source of color roles. The supplied conflux.qss supports Qt Widgets; Qt Quick should map the same roles into its own theme properties.",507,498,397,13,19,max_h=100)
label("WINDOWS AND SCALE",507,374,"lightAccent",10,397)
ptext("Keep Wayland and Xorg behavior within Qt's platform integration. Use scalable artwork or appropriate raster density; verify the launcher, title bar, tray and interface at real target scale factors.",507,345,397,13,19,max_h=100)
label("RELEASE REVIEW",507,224,"lightAccent",10,397)
ptext("Check light/dark variants, 100% and 200% scaling, keyboard focus, reduced motion, and an actual KDE session. The brand pack provides identity assets, not a completed desktop integration.",507,195,397,13,19,max_h=110)
ptext("Reference: KDE Human Interface Guidelines / Icons; freedesktop.org Icon Theme Specification. Links are listed on page 16.",M,75,848,10,14,color="lightMuted",max_h=22)
end()

# 16 - Asset handoff
page(16,"Assets and licensing","A usable system, ready to carry forward.","Keep the supplied masters and naming intact. Record product decisions before a public release.")
label("PACK CONTENTS",M,528,"lightAccent",10,400)
inventory=[("assets/logos/","Light/dark lockups, wordmarks and symbols; SVG masters and PNG companions."),
           ("assets/app-icons/","Full-color tiles and raster sizes from 16 to 1024 px."),
           ("assets/favicons/","SVG, ICO, touch and install icons; web manifest."),
           ("assets/fonts/","Barlow, Barlow Condensed, JetBrains Mono and accompanying OFL notices."),
           ("tokens/","Portable JSON, website CSS and Qt Widgets QSS theme tokens."),
           ("index.html / PDF","Brand website and this reference guide.")]
yy=497
for a,b in inventory:
    ptext(a,M,yy,398,11,15,font="Mono",max_h=20)
    ptext(b,M,yy-23,398,12,17,color="lightMuted",max_h=40);yy-=65
label("PERMISSIONS AND RELEASE STATUS",507,528,"lightAccent",10,397)
ptext("<b>No open-source software license has been selected in this brand pack.</b> The project's open-source intent does not itself grant software redistribution or modification rights. Choose and document the code license before release.",507,497,397,13,19,max_h=120)
ptext("The bundled fonts use the SIL Open Font License. Retain each font's bundled copyright and license notices when redistributing font files. The custom outlined wordmark is artwork, not a font file.",507,365,397,12,18,max_h=100)
ptext("Brand assets and software are separate licensing decisions. This guide does not establish trademark clearance, transfer rights, or grant an unrestricted public brand license. Record asset permissions explicitly in the project's release documentation.",507,258,397,12,18,max_h=100)
label("SOURCES AND REFERENCES",507,147,"lightMuted",9,397)
refs=[("Barlow / Barlow Condensed","https://github.com/jpt/barlow"),
      ("JetBrains Mono","https://github.com/JetBrains/JetBrainsMono"),
      ("KDE icon guidance","https://develop.kde.org/hig/icons/"),
      ("Icon Theme Specification","https://specifications.freedesktop.org/icon-theme/latest/"),
      ("WCAG 2.2 contrast guidance","https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum.html")]
for i,(a,u) in enumerate(refs):
    ptext(f'<link href="{u}" color="#5D626A">{escape(a)}</link>',507,128-i*14,397,9,12,color="lightMuted",max_h=14)
end()

cv.save()
assert CURRENT_PAGE == 16
print(json.dumps({"pdf":str(OUT),"pages":CURRENT_PAGE,"text_boxes":len(TEXT_BOXES),"draft":ARGS.draft},indent=2))
