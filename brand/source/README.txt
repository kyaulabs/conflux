Editable production sources / Version 1.1 / 15 October 2026

All logo SVGs are independent editable masters with outlined lettering. Brand fonts are in assets/fonts with their licenses. Website files are index.html, site.css and site.js. There is no build step; open index.html directly or serve this folder with a static HTTP server.

The approved version 1.1 symbol uses equal orbital currents related by an exact 180-degree rotation around (256, 250) in a 512 x 500 master. Its flat-top rounded hexagonal core uses radius 101 and an 18-unit corner setback with quadratic curves. The clear-space unit x is the core's flat-to-flat height, 101 x sqrt(3), approximately 174.94 master units. Minimum clear space is 0.5x. Measure between parallel flats, never across opposite points.

Optional regeneration (from the unpacked brand pack root):
  python3 source/build_assets.py .
  node source/rasterize.cjs .
  python3 source/build_guide.py

build_assets.py uses Python's standard library. rasterize.cjs requires the sharp Node package; install it in your development environment. It uses a temporary fontconfig configuration for the bundled social fonts. build_guide.py requires reportlab and Pillow. The PDF source auto-detects this unpacked folder. Source changes should precede regeneration; manual changes to generated assets are overwritten. QML/KDE templates are separate editable handoff files.

After changes, inspect SVG/PNG at actual use sizes, render every PDF page, and check the HTML in both themes and on mobile. Regenerate checksum and ZIP files for a new release. PNG icon sources include a separate simplified 16-24px variant. The ICO contains distinct 16/32/48px PNG entries.
