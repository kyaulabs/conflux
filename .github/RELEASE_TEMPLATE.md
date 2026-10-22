<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/{{REPOSITORY}}/{{TAG}}/brand/assets/social/social-banner-dark.png">
  <source media="(prefers-color-scheme: light)" srcset="https://raw.githubusercontent.com/{{REPOSITORY}}/{{TAG}}/brand/assets/social/social-banner-light.png">
  <img src="https://raw.githubusercontent.com/{{REPOSITORY}}/{{TAG}}/brand/assets/social/social-banner-light.png" alt="CONFLUX — Music, directed." width="100%">
</picture>

# CONFLUX {{VERSION}}

A native Linux DJ application built with C++20, Qt Quick and PipeWire.

Review the verification and remaining limitations below before using this release.
The [M2 live-stem gate](https://github.com/{{REPOSITORY}}/blob/{{TAG}}/docs/M2.md)
requires **4.095 seconds p99**. Completing M2 and the full
[M6 first-usable-release criteria](https://github.com/{{REPOSITORY}}/blob/{{TAG}}/docs/handoff/PLAN.md#implementation-sequence-and-exit-criteria)
requires their recorded acceptance evidence; a version tag does not waive those gates.

Changes since {{PREVIOUS}}.

{{CHANGES}}

{{COMPARE}}

## Build and run

Use Linux with a running PipeWire session. The window targets Wayland.
On Arch Linux, install the build dependencies:

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf pipewire libsndfile \
  libsamplerate rubberband taglib qt6-base qt6-declarative qt6-svg
```

Check the [tagged build instructions](https://github.com/{{REPOSITORY}}/blob/{{TAG}}/README.md#build-and-run)
for minimum dependency versions and contributor test requirements. Clone this
release and build the application:

```sh
git clone --branch {{TAG}} --depth 1 https://github.com/{{REPOSITORY}}.git conflux
cd conflux
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build/release --parallel 2
build/release/conflux
```

To install the executable, desktop entry, icon and license notices for your user:

```sh
cmake --install build/release
"$HOME/.local/bin/conflux"
```

This source build uses system libraries. The source archives below are not
self-contained application packages. Model weights, music, recordings and private
caches are not bundled. Follow the tagged instructions for the features supported
by this release.

## Update

Stop playback and recording, then close CONFLUX. Keep your original tracks,
recordings, library and settings. From a clean tracked checkout:

```sh
git fetch origin tag {{TAG}}
git switch --detach {{TAG}}
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build/release --parallel 2
cmake --install build/release
```

Reopen CONFLUX and confirm both decks, output routing and recording before a set.
Publishing a release does not install it or change a running player.

## Verification

{{VERIFICATION}}

GitHub provides source archives below.
