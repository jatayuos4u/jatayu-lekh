# Jatayu Lekh 0.2.5 development source

Jatayu Lekh is a dual-mode Qt 6 editor for JatayuOS.

## Build and install on Debian 13 or JatayuOS

```bash
sudo apt install build-essential cmake dpkg-dev qt6-base-dev \
  libqt6svg6 diffutils libhunspell-1.7-0 hunspell-en-us
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -B build/packages -G DEB
sudo apt install "./build/packages/jatayu-lekh_0.2.5_$(dpkg --print-architecture).deb"
```

Launch Jatayu Lekh from KDE or run `jatayu-lekh`.

## Changes in 0.2.5

- **Tools → Open HTML in Browser** opens a saved HTML file with your desktop's
  associated application. HTML rendering stays outside Lekh. Markdown Preview
  remains available in the editor.
- File reads reject special files and enforce size limits. Save checks again
  immediately before the atomic commit to narrow the external-change race.
- **File → Clear Recovery Data** clears this window's recovery file and old
  snapshot files after confirmation. Open drafts can be stored again while
  recovery is enabled. The serialized closed-tab archive is capped at 16 MiB
  for new additions, and new recovery files are capped at 128 MiB.
- Personal Dictionary has been removed. Standard English spellcheck,
  spelling suggestions and optional autocorrect are available.
- Snapshot commands, the History panel and persistent per-file snapshot
  creation have been removed. Earlier recovery records remain readable,
  and old per-file history files are not automatically deleted.
- Writing Assist has been removed.

## Editor behavior

- Text and Command modes share tabs, editing state and live word, character
  and paragraph counts. Character totals include spaces and line breaks.
  Blank lines separate paragraphs.
- Normal launches start with a blank tab. Use **File → Recover Draft** for
  unsaved edits or **Settings → Reopen previous tabs on startup**.
- **File → Review Disk Changes** compares the draft with the on-disk file,
  reloads it, keeps the draft, or saves a separate copy. The comparison is
  read-only. Change navigation supports up to 512 Ki UTF-16 code units per side.
- Choose **Settings → Language → हिन्दी**, then restart. Native system
  dialogs may use the desktop language.
- Files larger than 16 MiB open in a read-only UTF-8 viewer with search
  and **Follow End** for growing logs.

## Help menu links

The English and Hindi user guides open offline from Help or F1. Once the
repository and its chosen license are published, configure the Help links
using CMake options `-DLEKH_REPOSITORY_URL=<repository-url>` and
`-DLEKH_LICENSE_URL=<license-file-url>`. Use an HTTPS GitHub URL for each.

## License

Jatayu Lekh is distributed under the [MIT License](LICENSE).
