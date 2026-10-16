CONFLUX — BRAND PACK
Version 1.1 • 15 October 2026
Music, directed.

START HERE

Open index.html in a browser for the interactive identity board. Open
CONFLUX-brand-guide.pdf for the full design and implementation guide.
ASSET-REGISTER.md maps the supplied files to their intended uses.

This pack establishes the identity for an open-source Linux DJ application.
The app interface and website examples are design concepts. This pack does
not contain a working DJ application, audio engine, AI service or installer.
Integration templates are starting points for implementation, not proof that
an application, download, release, repository or service already exists.

THE IDENTITY

The approved version 1.1 refines the 01-C direction with equal orbital currents
and a centered, flat-top rounded hexagonal core. The lower current is an exact
180-degree rotation of the upper current around (256, 250) in the 512 x 500
symbol master. The upper current is Signal red; the lower current and core
are charcoal on light backgrounds. The tall condensed CONFLUX wordmark
retains the small Signal diamond inside the X.

The core is based on a regular flat-top hexagon of radius 101 master units.
Each corner is softened with an 18-unit setback and a quadratic curve. Use
the supplied path and preserve its center, dimensions and exact rounding.

Use the supplied outlined SVG wordmark. It is custom artwork, not a font.
Do not retype it, substitute letterforms, move the diamond, stretch the logo,
add outlines or shadows, or rearrange the mark's geometry.

CHOOSING A LOGO

  Files ending -light: for white, ivory and other light backgrounds.
  Files ending -dark:  for charcoal and other dark backgrounds. Charcoal
                       components reverse to light artwork for visibility.
  Files ending -black: one-color black artwork on a light background.
  Files ending -white: one-color white artwork on a dark background.

Use horizontal or stacked lockups when the name must be readable. Use the
symbol alone for launcher icons, avatars and very small placements. Use SVG
for scalable web, print and interface placement; PNG exports are ready-made
raster alternatives. The launcher tiles are square assets with their own
backgrounds, distinct from transparent logo exports.

CLEAR SPACE AND MINIMUM SIZES

Let x equal the hexagonal core's flat-to-flat height: 101 x sqrt(3), or about
174.94 master units. Minimum clear space is 0.5x (about 87.47 master units),
scaled with the mark. Keep that space around the entire lockup's outer visible
bounds. Measure between parallel flats, never across opposite points.
Do not place text, edges or controls in this area.

  Horizontal lockup: 180 CSS pixels wide / 40 mm in print.
  Stacked lockup:    140 CSS pixels wide / 32 mm in print.
  Wordmark alone:   120 CSS pixels wide / 28 mm in print.
  Standard symbol:   32 pixels minimum.
  Small symbol:      use the purpose-made simplified artwork at 16–24 px.

Do not reduce the full lockup to a favicon. The launcher mark occupies about
76% of its tile; leave the remaining safe area clear. Use the supplied launcher
tile without adding another frame. For surfaces that apply their own mask,
use the full-bleed maskable sources and preserve the safe area. Inspect print
reproduction and small raster exports at their actual output size before use.

COLOR AND ACCESSIBILITY

Signal #FF3030 is the identity accent. Charcoal #24252B and ivory #FAF9F6
provide the neutral foundation. Use the named light and dark theme tokens
from tokens/brand-tokens.json and tokens/brand.css rather than recoloring
every element with the logo's red.

Signal against white is 3.67:1: do not use that pairing for ordinary small
text. Use lightAccent #B92323 on light surfaces and darkAccent #FF877D on
dark surfaces. Use white labels on lightPrimary #C52524; use darkCanvas
#111317 labels on darkPrimary #FF5B50. White labels on darkPrimary or on
Signal do not meet the ordinary-text target used by this pack.

darkBorder #505660 is a decorative divider. Use darkControlBorder #78808A
for an essential control boundary; it remains at least 3:1 against the
defined dark canvas, surface and raised-surface colors. Focus indicators
also need a visible shape or outline, not a subtle color shift alone.

Deck colors and state colors have separate purposes. Pair deck identity with
an A/B label. Pair clipping, recording, success, warning, errors and planning
state with text, symbols, meter position or line style. Never rely on color
alone. A palette contrast check does not establish whole-product accessibility;
test the final type sizes, controls, focus order and rendered combinations.

TYPOGRAPHY

  Barlow Condensed Semibold: display headings and short emphasis.
  Barlow Regular/Medium/Semibold: body copy, track information and UI.
  JetBrains Mono Regular: time, BPM, bar counts and technical values.
  CONFLUX wordmark: outlined artwork only.

Keep dense track tables and body copy out of condensed display type. Enable
tabular numbers where supported. Respect user text scaling, system font and
accessibility preferences in the eventual application. Bundle fonts locally
when using the board or brand styles offline.

PRODUCT EXPRESSION

DIRECTOR handles selection, cues, transport, timing and crossfade. LIVE MIX
belongs to the performer: EQ and effects. Examples should make the next
musical action and control ownership clear. Use direct language such as
"Next transition in 16 bars" and "Preparing stems".

Keep animation restrained: about 160 ms for micro interactions and 240 ms
for panels. Respect reduced-motion preferences. The logo should not spin
continuously. Brand illustrations can be expressive; live controls should
remain stable, readable and predictable.

LINUX / KDE / WAYLAND INTEGRATION

integration/linux/ includes a hicolor icon tree, INSTALL.txt and the desktop
entry starter conflux.desktop.in. The template uses Exec=conflux and
Icon=conflux. Adapt it to the packaged binary and install the completed file
as conflux.desktop. See the included INSTALL.txt for packaging instructions.

Use the app icon assets in an application's packaging step. The following
paths are conventions to adapt, not actions this pack performs:

  PNG icons:
    ~/.local/share/icons/hicolor/SIZExSIZE/apps/APP_ID.png
  Scalable SVG icon, if used by the package:
    ~/.local/share/icons/hicolor/scalable/apps/APP_ID.svg
  Desktop entry:
    ~/.local/share/applications/APP_ID.desktop

APP_ID is a placeholder. The supplied files use the short name conflux.
If your packaging uses a reverse-DNS application ID, rename the desktop and
icon files and update Icon= (without an extension) consistently. Align the
application's desktop-file identity with the completed desktop entry.
Matching these identifiers helps KDE/Wayland associate windows, launchers
and task-manager icons.

A desktop entry must point Exec= at the actual installed executable and use
appropriate AudioVideo/Audio categories. System packages normally install
their icons and desktop entries under the matching /usr/share locations.
Flatpak and AppImage builds should follow their packaging tools' integration
rules instead of installing files into a user's home during build time.

Choose the dark or light launcher tile deliberately; either is a self-contained
icon treatment. Include multiple supplied sizes in the hicolor theme rather
than resizing a small PNG upward. Use the supplied small-size icon exports
where available.

The included integration/linux/hicolor tree uses the dark launcher treatment
and includes scalable and symbolic variants. The symbolic icon is separate
from the full-color launcher mark.

integration/kde/CONFLUX-Dark.colors and CONFLUX-Light.colors are installable
KDE color-scheme examples. To try them for your account, copy the .colors
files into ~/.local/share/color-schemes/ and select the scheme in KDE's color
settings. Honor user theme preferences.

tokens/conflux.qss is a Qt Widgets styling starter. Qt Quick/QML applications
should map the JSON/CSS token values into a QML palette or singleton; QSS is
not a QML theme. Keep native dialogs,
system integration and user accessibility preferences intact.
Explicit light and dark versions are supplied as tokens/conflux-light.qss
and tokens/conflux-dark.qss. tokens/CONFLUX.gpl is a GIMP palette file;
the .gpl extension here does not denote a GNU software license.
integration/qtquick/ supplies a Qt 6 palette singleton, qmldir and README.txt.
It is an untested runtime starter; validate it in the application project.

WEB INTEGRATION

Use the supplied favicon.svg and favicon.ico plus the touch/Android icons
and site.webmanifest. Keep their relative paths together when moving them
into a website. Update manifest identifiers, application URLs and any template
metadata to the real deployment.
integration/web-head.html supplies a starting HTML head snippet. Social
preview cards and banners are in assets/social; UI glyphs and their sprite
are in assets/ui-icons. Keep the glyphs' accessible text labels in the real UI.
Rendered identity boards and application concepts are in assets/previews.

LICENSES AND HANDOFF

Bundled Barlow, Barlow Condensed and JetBrains Mono fonts carry their own
SIL Open Font License files in assets/fonts. Preserve those notices when
redistributing the font files; consult the bundled terms before modifying or
renaming a font. The custom CONFLUX wordmark is not one of those fonts.
The exact notices are Barlow-OFL.txt, BarlowCondensed-OFL.txt and
JetBrainsMono-OFL.txt, all SIL Open Font License 1.1.

Font licenses apply to the font files. They are not the license for this
brand identity, a future application or its source code. The application
repository should declare its software license separately; this design pack
does not select one.

Source references and file purposes appear in ASSET-REGISTER.md and the PDF.
Retain the vector masters, theme tokens and bundled font notices together
when handing the identity to designers or developers.
source/README.txt describes the asset-build sources. The calculated palette
review is supplied in tokens/contrast-audit.json.
