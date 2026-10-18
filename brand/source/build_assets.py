"""CONFLUX 01-C identity: editable SVG masters and integration assets.
Run from the brand pack root or pass its directory as the first argument.
Raster exports are produced separately by rasterize.cjs.
"""
from pathlib import Path
import json, sys, shutil, html, math

ROOT = Path(sys.argv[1] if len(sys.argv)>1 else 'outputs/conflux-brand-pack').resolve()
SPEC = Path(__file__).parent/'brand-spec.json'
if not SPEC.exists(): SPEC = ROOT/'tokens/brand-tokens.json'
spec = json.loads(SPEC.read_text())
spec['colors']['darkControlBorder']='#78808A'
spec['colors']['deckALight']='#00786F'
c=spec['colors']
spec['root']='.'

def put(path, text):
    p=ROOT/path; p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)

def svg(w,h,body,label='CONFLUX'):
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" role="img" aria-label="{html.escape(label)}"><title>{html.escape(label)}</title>{body}</svg>'

# Approved 01-C refinement: matching currents and a rounded flat-top hex core.
# The lower current is the same path rotated exactly 180 degrees.
UPPER='M150 389 C0 353 -18 263 80 153 C180 42 333 -4 425 31 C463 45 478 80 450 111 C347 85 211 131 131 217 C61 292 65 352 150 389Z'
def rounded_hex(cx=256,cy=250,radius=101,setback=18):
    vertices=[(cx+radius*math.cos(math.radians(i*60)),cy+radius*math.sin(math.radians(i*60))) for i in range(6)]
    def toward(a,b):
        length=math.hypot(b[0]-a[0],b[1]-a[1])
        return (a[0]+setback*(b[0]-a[0])/length,a[1]+setback*(b[1]-a[1])/length)
    def xy(p):return f'{p[0]:.4f} {p[1]:.4f}'
    pieces=[]
    for i,v in enumerate(vertices):
        before=toward(v,vertices[(i-1)%6]);after=toward(v,vertices[(i+1)%6])
        pieces.append(('M' if i==0 else 'L')+xy(before)+'Q'+xy(v)+' '+xy(after))
    return ' '.join(pieces)+'Z'
HEX_CORE=rounded_hex()
def symbol(ink,accent,mono=False):
    return f'<g><path fill="{ink if mono else accent}" d="{UPPER}"/><path fill="{ink}" transform="rotate(180 256 250)" d="{UPPER}"/><path fill="{ink}" d="{HEX_CORE}"/></g>'

GLYPHS=[
('C',62,'M62 17C62 6 56 0 45 0H17C6 0 0 6 0 17V123C0 134 6 140 17 140H45C56 140 62 134 62 123V96H45V119C45 123 43 125 39 125H23C19 125 17 123 17 119V21C17 17 19 15 23 15H39C43 15 45 17 45 21V43H62Z'),
('O',62,'M17 0H45C56 0 62 6 62 17V123C62 134 56 140 45 140H17C6 140 0 134 0 123V17C0 6 6 0 17 0ZM23 15C19 15 17 17 17 21V119C17 123 19 125 23 125H39C43 125 45 123 45 119V21C45 17 43 15 39 15Z'),
('N',64,'M0 140V0H16L48 91V0H64V140H48L16 49V140Z'),
('F',54,'M0 0H54V17H17V61H49V78H17V140H0Z'),
('L',52,'M0 0H17V123H52V140H0Z'),
('U',64,'M0 0H17V117C17 122 20 125 25 125H39C44 125 47 122 47 117V0H64V119C64 133 57 140 43 140H21C7 140 0 133 0 119Z'),
('X',66,'M0 0H18L33 48L48 0H66L43 70L66 140H48L33 93L18 140H0L23 70ZM33 51L46 70L33 89L20 70Z')]
WORD_WIDTH=sum(g[1] for g in GLYPHS)+24*6
def wordmark(ink,accent,mono=False):
    parts=[];x=0
    for letter,w,d in GLYPHS:
        parts.append(f'<path transform="translate({x} 0)" fill="{ink}" fill-rule="evenodd" d="{d}"/>')
        if letter=='X': parts.append(f'<path transform="translate({x} 0)" fill="{ink if mono else accent}" d="M33 57L42 70L33 83L24 70Z"/>')
        x+=w+24
    return '<g>'+''.join(parts)+'</g>'

themes={'light':(c['charcoal'],c['signal'],False),'dark':(c['darkText'],c['signal'],False),'black':('#000000','#000000',True),'white':('#FFFFFF','#FFFFFF',True)}
for name,(ink,accent,mono) in themes.items():
    sy=symbol(ink,accent,mono);wm=wordmark(ink,accent,mono)
    put(f'assets/logos/conflux-symbol-{name}.svg',svg(512,500,sy,'CONFLUX symbol / '+name+' background'))
    put(f'assets/logos/conflux-wordmark-{name}.svg',svg(WORD_WIDTH,140,wm,'CONFLUX wordmark / '+name+' background'))
    put(f'assets/logos/conflux-stacked-{name}.svg',svg(640,740,f'<g transform="translate(64 8)">{sy}</g><g transform="translate({(640-WORD_WIDTH)/2} 558)">{wm}</g>','CONFLUX stacked logo'))
    put(f'assets/logos/conflux-horizontal-{name}.svg',svg(1200,360,f'<g transform="translate(8 12) scale(.66)">{sy}</g><g transform="translate(405 105) scale(1.3)">{wm}</g>','CONFLUX horizontal logo'))

# Small-format monochrome counterpart: fewer cubic segments, matching currents,
# centered rounded hex and half-turn symmetry on the 24 px grid.
SMALL_UPPER='M7 18 C0 16.5 -1 12.5 4 7.5 C8.5 2 15.5 0 20 1.5 C22 2 22.5 4 21 5.25 C16.25 4 10 6.25 6.25 10.25 C3 13.5 3 16.5 7 18Z'
symbolic=f'<path fill="currentColor" d="{SMALL_UPPER}"/><path fill="currentColor" transform="rotate(180 12 12)" d="{SMALL_UPPER}"/><path fill="currentColor" d="{rounded_hex(12,12,4.735,0.844)}"/>'
put('assets/logos/conflux-symbolic.svg',svg(24,24,symbolic,'CONFLUX symbolic icon'))
put('assets/app-icons/conflux-symbolic.svg',svg(24,24,symbolic,'CONFLUX symbolic icon'))
sizes=[16,22,24,32,48,64,128,256,512,1024]
for name,bg in [('dark',c['darkCanvas']),('light',c['ivory'])]:
    ink=c['darkText'] if name=='dark' else c['charcoal']
    body=f'<rect width="1024" height="1024" rx="218" fill="{bg}"/><g transform="translate(122.88 132) scale(1.52)">{symbol(ink,c["signal"])}</g>'
    put(f'assets/app-icons/conflux-app-{name}.svg',svg(1024,1024,body,'CONFLUX application icon'))
    small=f'<rect width="24" height="24" rx="5" fill="{bg}"/><g transform="translate(3 3) scale(.75)">{symbolic.replace("currentColor",ink)}</g>'
    put(f'assets/app-icons/conflux-app-{name}-small.svg',svg(24,24,small,'CONFLUX small application icon'))
    # Maskable assets require a full bleed background and a safe central mark.
    body=f'<rect width="512" height="512" fill="{bg}"/><g transform="translate(93.44 97.25) scale(.635)">{symbol(ink,c["signal"])}</g>'
    put(f'assets/favicons/maskable-{name}.svg',svg(512,512,body,'CONFLUX maskable icon'))

put('assets/favicons/favicon.svg','<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 500"><style>.ink{fill:#24252B}@media(prefers-color-scheme:dark){.ink{fill:#F5F4F0}}</style><path fill="#FF3030" d="'+UPPER+'"/><path class="ink" transform="rotate(180 256 250)" d="'+UPPER+'"/><path class="ink" d="'+HEX_CORE+'"/></svg>')
put('assets/favicons/safari-pinned-tab.svg',svg(24,24,symbolic.replace('currentColor','#000000')))
put('assets/favicons/site.webmanifest',json.dumps({'name':'CONFLUX','short_name':'CONFLUX','description':spec['tagline'],'start_url':'/','scope':'/','display':'standalone','background_color':c['darkCanvas'],'theme_color':c['darkCanvas'],'icons':[{'src':'android-chrome-192x192.png','sizes':'192x192','type':'image/png','purpose':'any'},{'src':'android-chrome-512x512.png','sizes':'512x512','type':'image/png','purpose':'any'},{'src':'maskable-512x512.png','sizes':'512x512','type':'image/png','purpose':'maskable'}]},indent=2))
put('integration/web-head.html','''<!-- Resolve these paths for your deployed site. Customize manifest start_url and scope. -->
<link rel="icon" href="assets/favicons/favicon.svg" type="image/svg+xml">
<link rel="icon" href="assets/favicons/favicon.ico" sizes="16x16 32x32 48x48">
<link rel="apple-touch-icon" href="assets/favicons/apple-touch-icon.png">
<link rel="mask-icon" href="assets/favicons/safari-pinned-tab.svg" color="#FF3030">
<link rel="manifest" href="assets/favicons/site.webmanifest">
<meta name="theme-color" content="#111317" media="(prefers-color-scheme: dark)">
<meta name="theme-color" content="#FAF9F6" media="(prefers-color-scheme: light)">
<!-- Replace the canonical URL and use an absolute og:image URL when shipping. -->
<meta property="og:title" content="CONFLUX — Music, directed.">
<meta property="og:description" content="Arrange the music. Shape the moment.">
<meta property="og:image" content="assets/social/conflux-open-graph-dark.png">
<meta name="twitter:card" content="summary_large_image">
''')

# Application icon vocabulary: 24 px grid, 2 px stroke, round caps and joins.
icons={
'play':'<path d="m8 5 11 7-11 7Z"/>', 'pause':'<path d="M8 5v14M16 5v14"/>',
'stop':'<rect x="6" y="6" width="12" height="12" rx="1"/>',
'cue':'<path d="M5 5v14M19 5l-10 7 10 7Z"/>',
'loop':'<path d="M4 9h14l-3-3M20 15H6l3 3M18 9a3 3 0 0 1 3 3M6 15a3 3 0 0 1-3-3"/>',
'sync':'<path d="M4 8a8 8 0 0 1 14-2l2 2M20 3v5h-5M20 16a8 8 0 0 1-14 2l-2-2M4 21v-5h5"/>',
'headphones':'<path d="M4 14v-3a8 8 0 0 1 16 0v3"/><rect x="3" y="12" width="5" height="8" rx="2"/><rect x="16" y="12" width="5" height="8" rx="2"/>',
'microphone':'<rect x="9" y="3" width="6" height="12" rx="3"/><path d="M5 11a7 7 0 0 0 14 0M12 18v3M8 21h8"/>',
'filter':'<path d="M3 5h18l-7 8v6l-4 2v-8Z"/>',
'fx':'<path d="m13 2-8 12h7l-1 8 8-12h-7Z"/>',
'equalizer':'<path d="M5 3v7m0 4v7M12 3v3m0 4v11M19 3v11m0 4v3M3 10h4v4H3ZM10 6h4v4h-4ZM17 14h4v4h-4Z"/>',
'settings':'<path d="M9 3h6l.7 3 2.5 1.5L21 7l2 5-2.3 2 .1 3-4.6 2.7-2.2-2h-3l-2.2 2L4.2 17l.1-3L2 12l2-5 2.8.5L9 6Z" transform="translate(0 -1) scale(.96)"/><circle cx="12" cy="11" r="3"/>',
'library':'<rect x="3" y="4" width="4" height="16" rx="1"/><rect x="10" y="4" width="4" height="16" rx="1"/><path d="m17 5 4-1 3 15-4 1Z" transform="translate(-1)"/>',
'search':'<circle cx="10" cy="10" r="6"/><path d="m15 15 6 6"/>',
'close':'<path d="m6 6 12 12M18 6 6 18"/>',
'check':'<path d="m4 12 5 5L20 6"/>',
'warning':'<path d="m12 3 10 18H2ZM12 9v5M12 17v.1"/>',
'volume':'<path d="M3 9h4l5-5v16l-5-5H3ZM16 8a6 6 0 0 1 0 8M19 5a10 10 0 0 1 0 14"/>',
'mute':'<path d="M3 9h4l5-5v16l-5-5H3ZM17 9l5 6M22 9l-5 6"/>',
'folder':'<path d="M3 7V5h7l2 2h9v13H3Z"/>',
'download':'<path d="M12 3v12m-5-5 5 5 5-5M4 16v5h16v-5"/>',
'external-link':'<path d="M14 3h7v7M21 3 10 14M10 3H3v18h18v-7"/>',
'director':'<path d="m12 2 2.4 7.6L22 12l-7.6 2.4L12 22l-2.4-7.6L2 12l7.6-2.4Z"/>',
'user':'<circle cx="12" cy="7" r="4"/><path d="M4 21v-2a8 8 0 0 1 16 0v2"/>',
'midi':'<circle cx="12" cy="12" r="9"/><path d="M7 17h10M7 10v.1M10 7v.1M14 7v.1M17 10v.1M12 12v.1"/>',
'record':'<circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="3" fill="currentColor"/>',
'eject':'<path d="m12 4 8 11H4ZM4 20h16"/>',
'beatgrid':'<path d="M3 5v14M9 8v8M15 5v14M21 8v8M3 12h18"/>',
'chevron-right':'<path d="m9 5 7 7-7 7"/>',
'chevron-down':'<path d="m5 9 7 7 7-7"/>',
'sun':'<circle cx="12" cy="12" r="4"/><path d="M12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.5 1.5m11 11L19 19M5 19l1.5-1.5m11-11L19 5"/>',
'moon':'<path d="M21 13.5A9 9 0 0 1 10.5 3 9 9 0 1 0 21 13.5Z"/>',
}
sprite=[]
for name,body in icons.items():
    style='fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"'
    put(f'assets/ui-icons/{name}.svg',svg(24,24,f'<g {style}>{body}</g>',name.replace('-',' ')))
    sprite.append(f'<symbol id="{name}" viewBox="0 0 24 24"><g {style}>{body}</g></symbol>')
put('assets/ui-icons/sprite.svg','<svg xmlns="http://www.w3.org/2000/svg">'+''.join(sprite)+'</svg>')
put('assets/ui-icons/README.txt','32 original interface icons. 24 x 24 viewBox; 2 px stroke; round joins and caps. Recolor with CSS currentColor. Use alongside visible text or an accessible name. Stroke icons should generally remain >= 20 px. CONFLUX symbolic launcher asset is separate.\n')

# Social art uses the outlined lockup, so exported rasters do not rely on fonts.
for theme in ['dark','light']:
    bg=c[theme+'Canvas'];ink=c['darkText'] if theme=='dark' else c['charcoal'];muted=c[theme+'Muted']
    for form,w,h in [('open-graph',1200,630),('banner',1500,500)]:
        body=f'<rect width="{w}" height="{h}" fill="{bg}"/><path d="M{w*.5} -100C{w*1.2} -30 {w*1.2} {h*1.3} {w*.6} {h*1.3}" fill="none" stroke="{ink}" stroke-opacity=".055" stroke-width="80"/>'
        body+=f'<rect x="64" y="64" width="36" height="4" fill="{c["signal"]}"/>'
        if form=='open-graph':
            body+=f'<g transform="translate(70 132) scale(.62)">{symbol(ink,c["signal"])}</g><g transform="translate(453 211) scale(1.12)">{wordmark(ink,c["signal"])}</g>'
            body+=f'<text x="459" y="422" font-family="Barlow, sans-serif" font-size="32" fill="{ink}">Music, directed.</text><text x="459" y="468" font-family="Barlow, sans-serif" font-size="21" fill="{muted}">OPEN-SOURCE DJ SOFTWARE FOR LINUX</text>'
        else:
            body+=f'<g transform="translate(60 64) scale(.7)">{symbol(ink,c["signal"])}</g><g transform="translate(480 126) scale(1.3)">{wordmark(ink,c["signal"])}</g><text x="482" y="366" font-family="Barlow, sans-serif" font-size="32" fill="{muted}">Music, directed.</text>'
        path=f'assets/social/conflux-open-graph-{theme}.svg' if form=='open-graph' else f'assets/social/social-banner-{theme}.svg'
        put(path,svg(w,h,body,'CONFLUX / Music, directed.'))
    pat=f'<rect width="1200" height="800" fill="{bg}"/><g fill="none" stroke="{ink}" stroke-opacity=".08" stroke-width="2">'+''.join(f'<ellipse cx="600" cy="400" rx="{n*72}" ry="{n*35}" transform="rotate(-28 600 400)"/>' for n in range(2,13))+'</g><path d="M590 380L604 400L590 420L576 400Z" fill="#FF3030"/>'
    put(f'assets/patterns/conflux-current-{theme}.svg',svg(1200,800,pat,'CONFLUX current pattern'))

def kebab(s):
    return ''.join('-'+k.lower() if k.isupper() else k for k in s)
fontcss='\n'.join(f'@font-face{{font-family:"{family}";src:url("../assets/fonts/{file}.ttf") format("truetype");font-weight:{weight};font-style:normal;font-display:swap}}' for family,file,weight in [('Barlow','Barlow-Regular',400),('Barlow','Barlow-Medium',500),('Barlow','Barlow-SemiBold',600),('Barlow Condensed','BarlowCondensed-SemiBold',600),('JetBrains Mono','JetBrainsMono-Regular',400)])
css=fontcss+'\n:root{\n'+''.join(f'  --conflux-{kebab(k)}: {v};\n' for k,v in c.items())+'  --font-ui:"Barlow",sans-serif;--font-display:"Barlow Condensed",sans-serif;--font-mono:"JetBrains Mono",monospace;\n  --radius-control:6px;--radius-panel:12px;--space-unit:4px;--motion-fast:160ms;--motion-panel:240ms;\n}\n'
for t,sel in [('dark',':root,[data-theme="dark"]'),('light','[data-theme="light"]')]:
    roles={'canvas':t+'Canvas','surface':t+'Surface','raised':t+'Raised','text':t+'Text','muted':t+'Muted','accent':t+'Accent','border':t+'Border','control-border':('darkControlBorder' if t=='dark' else 'lightBorder'),'primary':t+'Primary','deck-a':'deckA'+t.title(),'deck-b':'deckB'+t.title(),'success':'success'+t.title(),'warning':'warning'+t.title()}
    css+=sel+'{color-scheme:'+t+';'+''.join(f'--{k}:var(--conflux-{kebab(v)});' for k,v in roles.items())+'}\n'
css+='@media(prefers-reduced-motion:reduce){:root{--motion-fast:0ms;--motion-panel:0ms}}\n'
put('tokens/brand.css',css)
put('tokens/brand-tokens.json',json.dumps(spec,indent=2)+'\n')
for theme in ['dark','light']:
    prefix=theme
    bg=c[prefix+'Canvas'];surface=c[prefix+'Surface'];text=c[prefix+'Text'];border=c['darkControlBorder' if theme=='dark' else 'lightBorder'];accent=c[prefix+'Accent'];primary=c[prefix+'Primary']
    qss=f'''/* CONFLUX {theme} — Qt Widgets starter theme. For Qt Quick bind tokens explicitly. */
QWidget {{ background: {bg}; color: {text}; font-family: "Barlow"; font-size: 14px; }}
QFrame[panel="true"], QMenu {{ background: {surface}; border: 1px solid {border}; border-radius: 12px; }}
QPushButton {{ background: {surface}; border: 1px solid {border}; border-radius: 6px; padding: 8px 16px; min-height: 20px; }}
QPushButton:hover {{ border-color: {accent}; }}
QPushButton:focus, QLineEdit:focus {{ border: 2px solid {accent}; }}
QPushButton[primary="true"] {{ background: {primary}; color: {'#111317' if theme=='dark' else '#FFFFFF'}; border-color: {primary}; }}
QPushButton:disabled {{ color: {c[prefix+'Muted']}; background: {c[prefix+'Raised']}; }}
QLineEdit, QComboBox, QListView {{ background: {surface}; border: 1px solid {border}; border-radius: 6px; padding: 8px; selection-background-color: {primary}; selection-color: {'#111317' if theme=='dark' else '#FFFFFF'}; }}
QLabel[muted="true"] {{ color: {c[prefix+'Muted']}; }}
QSlider::groove:horizontal {{ height: 4px; background: {border}; border-radius: 2px; }}
QSlider::handle:horizontal {{ width: 16px; margin: -6px 0; background: {text}; border-radius: 8px; }}
QProgressBar {{ border: 1px solid {border}; border-radius: 4px; text-align: center; }}
QProgressBar::chunk {{ background: {accent}; }}
QToolTip {{ background: {surface}; color: {text}; border: 1px solid {border}; padding: 6px; }}
'''
    put(f'tokens/conflux-{theme}.qss',qss)
    put(f'integration/kde/CONFLUX-{theme.title()}.colors',f'''[General]
Name=CONFLUX {theme.title()}
ColorScheme=CONFLUX{theme.title()}
shadeSortColumn=true
[Colors:Window]
BackgroundNormal={','.join(str(int(bg[i:i+2],16)) for i in (1,3,5))}
ForegroundNormal={','.join(str(int(text[i:i+2],16)) for i in (1,3,5))}
[Colors:View]
BackgroundNormal={','.join(str(int(surface[i:i+2],16)) for i in (1,3,5))}
ForegroundNormal={','.join(str(int(text[i:i+2],16)) for i in (1,3,5))}
[Colors:Button]
BackgroundNormal={','.join(str(int(surface[i:i+2],16)) for i in (1,3,5))}
ForegroundNormal={','.join(str(int(text[i:i+2],16)) for i in (1,3,5))}
[Colors:Selection]
BackgroundNormal={','.join(str(int(primary[i:i+2],16)) for i in (1,3,5))}
ForegroundNormal={'17,19,23' if theme=='dark' else '255,255,255'}
''')
put('tokens/conflux.qss',(ROOT/'tokens/conflux-dark.qss').read_text())
gpl='GIMP Palette\nName: CONFLUX 01-C\nColumns: 4\n#\n'+''.join(' '.join(str(int(v[i:i+2],16)).rjust(3) for i in (1,3,5))+'\t'+k+'\n' for k,v in c.items())
put('tokens/CONFLUX.gpl',gpl)
put('integration/linux/conflux.desktop.in','''[Desktop Entry]
Name=CONFLUX
GenericName=DJ Performance
Comment=Music, directed.
Exec=conflux
Icon=conflux
Type=Application
Terminal=false
Categories=AudioVideo;Audio;Midi;
Keywords=DJ;Mixing;Music;
''')
put('integration/linux/INSTALL.txt','''Packaging template, not an installer. When shipping the application, install conflux.desktop.in as share/applications/conflux.desktop and copy hicolor/ into share/icons/hicolor/. Keep Icon=conflux extensionless. Verify Exec=conflux against the packaged binary. For Flatpak choose an application ID you own, rename the desktop/icon files to that ID, and update Icon accordingly. No ownership of a reverse-DNS namespace is assumed here. Keep scalable symbolic icons theme-colored. Qt Widgets QSS is a starter; Qt Quick/QML must map tokens to its own palette/components. Respect the desktop theme, keyboard navigation, high contrast and reduced motion. This brand pack does not implement Wayland windowing or an audio engine.\n''')
src=ROOT/'source';src.mkdir(exist_ok=True)
if Path(__file__).resolve()!=src/'build_assets.py':shutil.copy2(__file__,src/'build_assets.py')
print('Created SVG masters, 32 UI icons, themes, social artwork, palettes and integration templates.')
