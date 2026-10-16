Qt Quick starter module / Qt 6

Copy the module into the application's QML import path, or register these files with qt_add_qml_module using URI Conflux.Brand. Import Conflux.Brand and bind component colors to ConfluxTheme.canvas, text, primary, controlBorder, etc. Set ConfluxTheme.dark from the user's theme preference and ConfluxTheme.reducedMotion from the accessibility preference. Fonts must be registered with QFontDatabase or FontLoader using the bundled TTF files. Include their OFL notices in distributions.

These are palette and timing definitions, not a complete Qt Quick Controls style. Keep state feedback, keyboard focus, disabled behavior, input method support and accessibility in the application's actual components. The QSS files apply to Qt Widgets only. No Qt runtime was used to validate these starter templates.

For Wayland launcher grouping, use a consistent application/desktop file ID owned by your project, match the packaged desktop file basename, and configure QGuiApplication::setDesktopFileName appropriately. Set QIcon::fromTheme to the installed icon basename. Do not infer a reverse-DNS ID from this example pack. Validate in a real KDE Wayland session before release.
