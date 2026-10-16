# CONFLUX asset register

Version 1.1 · 15 October 2026. Paths are relative to this brand-pack folder.

This pack defines the approved symmetric rounded-hex refinement of the 01-C identity and demonstrates its use. The application and website visuals are concepts. Integration templates, icons, manifests and styling starters do not include a working DJ application or establish that a downloadable release or hosted service exists.

## Start here

| File | Purpose |
|---|---|
| `index.html` | Interactive identity board and website/application concept examples. |
| `CONFLUX-brand-guide.pdf` | Full brand system and implementation guide for designers and developers. |
| `README.txt` | Plain-text usage, theme selection, accessibility, Linux integration and licensing notes. |
| `ASSET-REGISTER.md` | This file map and handoff reference. |

## Logo masters

The theme suffix describes the **intended background**: `-light` assets go on light backgrounds; `-dark` assets go on dark backgrounds. The dark versions reverse the charcoal elements to light artwork while retaining the Signal-red accent.

| SVG master | Intended use |
|---|---|
| `assets/logos/conflux-symbol-light.svg` | Standalone color symbol on white or ivory. |
| `assets/logos/conflux-symbol-dark.svg` | Standalone color symbol on charcoal or dark surfaces. |
| `assets/logos/conflux-symbol-black.svg` | One-color black mark on a light background. |
| `assets/logos/conflux-symbol-white.svg` | One-color white mark on a dark background. |
| `assets/logos/conflux-wordmark-light.svg` | Custom outlined CONFLUX wordmark on a light background. |
| `assets/logos/conflux-wordmark-dark.svg` | Reversed wordmark on a dark background. |
| `assets/logos/conflux-stacked-light.svg` | Symbol over wordmark on a light background. |
| `assets/logos/conflux-stacked-dark.svg` | Symbol over wordmark on a dark background. |
| `assets/logos/conflux-horizontal-light.svg` | Symbol beside wordmark on a light background. |
| `assets/logos/conflux-horizontal-dark.svg` | Symbol beside wordmark on a dark background. |

Black and white variants are also supplied for the wordmark, stacked and horizontal lockups, with the same `-black` / `-white` suffix convention. `assets/logos/conflux-symbolic.svg` is the simplified monochrome symbol. Each logo SVG has a same-basename PNG counterpart.

Use SVG as the scalable master. PNG counterparts are raster conveniences, not independent logo designs. The wordmark is artwork rather than live text; its red diamond is part of the X. Do not substitute a font, change the geometry or move that detail.

Version 1.1 uses equal currents related by an exact **180-degree rotation around (256, 250)** in the **512 x 500** symbol master. The centered core is a **flat-top regular hexagon with radius 101**, softened by an **18-unit corner setback and quadratic curves**. Retain the supplied path and preserve its center, dimensions and exact rounding.

Keep clear space of at least **0.5x**, where **x is the core's flat-to-flat height: 101 x sqrt(3), approximately 174.94 master units**. Minimum clear space is therefore approximately **87.47 master units**, scaled with the mark and applied around the whole lockup's outer visible bounds. Measure between parallel flats, never across opposite points. Minimum widths are **180 px / 40 mm** for the horizontal lockup, **140 px / 32 mm** for the stacked lockup, and **120 px / 28 mm** for the wordmark. The standard symbol starts at **32 px**; use purpose-made simplified artwork at **16–24 px**.

## App icons

| Pattern | Purpose |
|---|---|
| `assets/app-icons/conflux-app-dark-SIZE.png` | Launcher tile with a dark background. |
| `assets/app-icons/conflux-app-light-SIZE.png` | Launcher tile with a light background. |
| `assets/app-icons/conflux-app-dark.svg` and `conflux-app-light.svg` | Scalable launcher-tile masters. |
| `assets/app-icons/conflux-app-dark-small.svg` and `conflux-app-light-small.svg` | Simplified source artwork for small icon sizes. |
| `assets/app-icons/conflux-symbolic.svg` | Monochrome symbolic launcher icon. |

The supplied size set is **16, 22, 24, 32, 48, 64, 128, 256, 512 and 1024 px**. Replace `SIZE` with the chosen size. Launcher tiles include their own background and corner treatment; they are distinct from transparent symbol masters. Use them without adding another frame. On surfaces that apply their own mask, use the full-bleed maskable sources and preserve the safe area.

For a Linux package, map the PNG sizes to `hicolor/SIZExSIZE/apps/APP_ID.png`. Use one stable application ID across icon names, desktop entry, package metadata and the application's desktop-file identity. `APP_ID` is a placeholder, not a registered project identifier. See `README.txt` for integration details.

## Website icons

| File | Purpose |
|---|---|
| `assets/favicons/favicon.svg` | Scalable browser favicon. |
| `assets/favicons/favicon.ico` | Legacy/multi-resolution browser icon. |
| `assets/favicons/apple-touch-icon.png` | Touch/home-screen icon. |
| `assets/favicons/android-chrome-192x192.png` | 192 px manifest icon. |
| `assets/favicons/android-chrome-512x512.png` | 512 px manifest icon. |
| `assets/favicons/site.webmanifest` | Website manifest starter; adapt paths and metadata for the real site. |
| `assets/favicons/favicon-16.png`, `favicon-32.png`, `favicon-48.png` | Explicit small browser-icon raster exports. |
| `assets/favicons/safari-pinned-tab.svg` | Monochrome pinned-tab mark. |
| `assets/favicons/maskable-512x512.png` | Mask-safe manifest icon. |
| `assets/favicons/maskable-dark.svg`, `maskable-light.svg` | Dark/light maskable-icon vector sources. |

## Social and supporting artwork

| Pattern | Purpose |
|---|---|
| `assets/social/conflux-open-graph-{dark,light}.{svg,png}` | Dark/light social preview cards. |
| `assets/social/social-banner-{dark,light}.{svg,png}` | Wide dark/light brand banners. |
| `assets/social/conflux-avatar-{dark,light}.png` | Square profile/avatar exports. |
| `assets/patterns/conflux-current-{dark,light}.svg` | Supporting current/orbit patterns for branded backgrounds. |
| `assets/previews/application-concept-{dark,light}.png` | Rendered app concepts in both themes. |
| `assets/previews/identity-board-{dark,light}.png` | Rendered identity-board previews in both themes. |

The brace notation lists alternatives; the actual filenames contain `dark` or `light`, and `svg` or `png`.

## Interface glyphs

`assets/ui-icons/` contains **32 original 24 × 24 SVG glyphs**, `sprite.svg`, and a usage `README.txt`. The `png-dark/` and `png-light/` folders provide themed raster counterparts. The SVG glyphs use `currentColor`; the theme suffix on PNG folders again denotes the intended background. These are interface icons, separate from the CONFLUX brand symbol. Keep stroke glyphs at 20 px or larger in the interface and give interactive icons a visible label or accessible name.

## Theme and implementation tokens

| File | Purpose |
|---|---|
| `tokens/brand-tokens.json` | Machine-readable identity, palette, typography roles and usage rules. |
| `tokens/brand.css` | CSS theme variables and brand styling starter. |
| `tokens/conflux.qss` | Qt Widgets styling starter; not a QML theme or complete application skin. |
| `tokens/conflux-dark.qss`, `tokens/conflux-light.qss` | Explicit dark/light Qt Widgets starter styles. |
| `tokens/CONFLUX.gpl` | GIMP palette for design tools. This file extension does not denote a GNU software license. |
| `tokens/contrast-audit.json` | Calculated sRGB contrast review: all 56 intended token-pair checks pass. |

Use **Signal `#FF3030`** for the identity. Use theme-specific `lightAccent` / `darkAccent` values for ordinary red text. Primary-button labels are white on `lightPrimary` and dark-canvas ink on `darkPrimary`.

`darkBorder` (`#505660`) is a decorative separator. **`darkControlBorder` (`#78808A`) is the essential-control outline token**; it exceeds 3:1 against all three defined dark surfaces. Do not silently substitute the decorative border for required control boundaries.

Deck A/B colors and success/warning tokens express function, not a secondary logo palette. Pair them with labels or symbols. Distinguish proposed and committed arrangements through wording and line treatment; preserve the performer's EQ/effect control ownership.

The contrast review concerns explicit, opaque foreground/background pairs. It does not certify the whole application, typography, keyboard behavior or every future composition. Verify the actual rendered controls and small icon exports.

## Platform and website integration

| File or folder | Purpose and status |
|---|---|
| `integration/linux/conflux.desktop.in` | Desktop-entry packaging template using `Exec=conflux` and `Icon=conflux`; adapt to the real installed executable. |
| `integration/linux/INSTALL.txt` | Linux icon/desktop-entry packaging instructions. |
| `integration/linux/hicolor/SIZExSIZE/apps/conflux.png` | Dark launcher treatment arranged in a hicolor icon tree at the supplied raster sizes. |
| `integration/linux/hicolor/scalable/apps/conflux.svg` | Scalable full-color launcher icon. |
| `integration/linux/hicolor/symbolic/apps/conflux-symbolic.svg` | Theme-compatible symbolic icon. |
| `integration/kde/CONFLUX-Dark.colors` | Installable KDE dark color-scheme example. |
| `integration/kde/CONFLUX-Light.colors` | Installable KDE light color-scheme example. |
| `integration/web-head.html` | Favicon, manifest, theme-color and social metadata snippet for adaptation. |
| `integration/qtquick/ConfluxTheme.qml` | Qt 6 palette singleton starter; runtime integration is untested. |
| `integration/qtquick/qmldir` | Module declaration for the singleton. |
| `integration/qtquick/README.txt` | Qt Quick integration instructions and scope. |

The KDE color schemes can be copied into `~/.local/share/color-schemes/` and selected through KDE's color settings. Rename the desktop/icon files consistently when adopting a reverse-DNS application ID, and align the app's desktop-file identity so KDE/Wayland can associate the window and launcher.

## Fonts and their licenses

| Font file | Role |
|---|---|
| `assets/fonts/Barlow-Regular.ttf` | Body copy and regular interface text. |
| `assets/fonts/Barlow-Medium.ttf` | Interface emphasis and labels. |
| `assets/fonts/Barlow-SemiBold.ttf` | Strong labels and headings. |
| `assets/fonts/BarlowCondensed-SemiBold.ttf` | Short display headings; not the logo wordmark. |
| `assets/fonts/JetBrainsMono-Regular.ttf` | Time, BPM, bar counts and technical readouts. |
| `assets/fonts/Barlow-OFL.txt` | SIL Open Font License 1.1 notice for Barlow. |
| `assets/fonts/BarlowCondensed-OFL.txt` | SIL Open Font License 1.1 notice for Barlow Condensed. |
| `assets/fonts/JetBrainsMono-OFL.txt` | SIL Open Font License 1.1 notice for JetBrains Mono. |

The accompanying SIL Open Font License notices in `assets/fonts` apply to those font files. Preserve them with redistributed fonts. The font licenses do **not** select a license for the CONFLUX identity, the future application or its source code. The custom wordmark is separate outlined artwork.

## Build sources and review

| File | Purpose |
|---|---|
| `source/README.txt` | Rebuild notes for the supporting asset sources. |
| `source/build_assets.py` | Vector, token and integration-asset generator. |
| `source/rasterize.cjs` | Raster-export source. |
| `source/build_guide.py` | Portable 16-page PDF generator using the current logo PNGs and bundled fonts. |
| `source/asset-qa.txt` | Independent structural, dimension, asset-reference and small-icon review. |

## Product language

The product concept divides ownership clearly: **DIRECTOR** handles selection, cues, transport, timing and crossfade; **LIVE MIX** belongs to the performer for EQ and effects. Favor concise musical feedback such as “Next transition in 16 bars.” Respect reduced motion, user text scaling and accessibility settings.

## Source references

- [Barlow and Barlow Condensed](https://github.com/jpt/barlow)
- [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono)
- [KDE icon guidance](https://develop.kde.org/hig/icons/)
- [Freedesktop icon-theme specification](https://specifications.freedesktop.org/icon-theme/latest/)

The references explain the supporting fonts and platform conventions. They do not imply endorsement of CONFLUX by those projects.
