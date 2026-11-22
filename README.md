# Jatayu Lekh

A lightweight Qt 6 text editor for **JatayuOS**, with a simple Text mode and a Command mode for quick access to editing tools.

## Download

**Stable release v0.2.5** · Debian 13 and JatayuOS · AMD64 (x86-64)

**[Download Jatayu Lekh (.deb)](https://github.com/jatayuos4u/jatayu-lekh/releases/download/v0.2.5/jatayu-lekh_0.2.5_amd64.deb)**

[Release notes and source archives](https://github.com/jatayuos4u/jatayu-lekh/releases/tag/v0.2.5) · [SHA-256 checksums](https://github.com/jatayuos4u/jatayu-lekh/releases/download/v0.2.5/SHA256SUMS)

### Install

Download the package and `SHA256SUMS` into the same folder. Open a terminal in that folder and run:

```bash
sha256sum -c SHA256SUMS &&
sudo apt install ./jatayu-lekh_0.2.5_amd64.deb
```

Open **Jatayu Lekh** from the KDE application menu or run `jatayu-lekh`.

## Features

- **Text and Command modes** with shared tabs, split editing, line numbers, and a command palette.
- **Editing tools** including Find and Replace, English spellcheck, and live word, character, and paragraph counts.
- **Draft recovery and disk-change review** to recover unsaved work or compare a draft with a file changed by another app.
- **Markdown preview** beside the source, browser opening for saved HTML, and PDF export through the Print dialog.
- **Large-file viewer** with search and Follow End for growing logs.
- **English and Hindi interface and offline user guide.** Press **F1** to open the guide.

## Documentation and support

[English user guide](docs/help-en.md) · [हिन्दी उपयोगकर्ता मार्गदर्शिका](docs/help-hi.md) · [Report a bug](https://github.com/jatayuos4u/jatayu-lekh/issues) · [Report a security issue](SECURITY.md)

## Build from source

```bash
sudo apt install build-essential cmake dpkg-dev file qt6-base-dev \
  libqt6svg6 diffutils libhunspell-1.7-0 hunspell-en-us
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -B build/packages -G DEB
```

The resulting package is in `build/packages/`.

## License

Released under the [MIT License](LICENSE). Created by Anup Kumar Yadav for [JatayuOS](https://jatayuos.in/).
