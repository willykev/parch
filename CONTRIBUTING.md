# Contributing to parch

Bug reports and small improvements are welcome. Open an
[issue](https://github.com/willykev/parch/issues) to discuss a larger change first.

## Build and test

On Arch Linux, install `base-devel`, `qt6-base`, `qt6-declarative`,
`qt6-svg`, `qt6-wayland`, `fontconfig`, `hunspell`, `libyaml` and
`syntax-highlighting`. parch also uses `xdg-desktop-portal` for file dialogs
and the `hunspell-en_us` dictionary for spell checking; the tests bring a
small dictionary of their own.

```sh
bin/build
bin/test
build/parch
```

`bin/fetch-helpers` downloads the pinned math and diagram helpers, and
`bin/fetch-test-fixtures` the CSL test suite's fixtures, into `build/`; the
tests that need them are skipped until they are there.

The tests run offscreen with temporary folders and never touch your own
documents, settings or theme. One test runs a whole benchmark and takes about
90 seconds; it runs only with `PARCH_SLOW_TESTS=1 bin/test`.

Keep changes focused, add a regression test for a bug fix, and describe how
you checked the result in your pull request. Follow the surrounding C++ and
QML style. Saving, recovery and conflicts deserve extra care: a change there
should come with a test that shows the text cannot be lost.

## Measuring

`bin/benchmark OUTDIR` measures parch in its real window, with live
formatting and spelling on, on generated documents: a typical 10,000-word
document, a 100,000-word book, one 10,000-word paragraph, and a rich
document with pictures, a 200-row table, 50 formulas, 10 diagrams, two
included chapters and a bibliography. Each run records:

- the launch to an editable window, and the restoring of a recovery draft;
- typing at three places: each key's time to its frame and on the UI
  thread, frames that missed the display's refresh, and the CPU time and
  parses each key costs until every view of the text has caught up;
- scrolling, jumps and re-wrapping, against a plain control scene, with
  dropped frames;
- a minute idle, from the moment every view has caught up and the caret has
  come to rest: frames besides the caret, CPU ticks and wakeups of every
  thread, the helpers parch starts included;
- memory, with those helpers.

The standard matrix is the typical document, the book and the long
paragraph as source and beside the preview, the rich document beside the
preview, and the typical document beside the preview in two windows, and
with adversarial edits (a 1 MB paste and its undo, nesting 200 deep, and
keys faster than any view settles), three runs each, about 45 minutes in
all. `OUTDIR/summary.md` has
the medians against the performance gates. `--view source|split|reading`,
`--windows 2`, `--adversarial`, `--document` and `--runs` measure one mode
instead. The script needs Python 3.

Run it in a desktop session with the screen awake, and leave its windows in
front until it ends. `--offscreen` runs it without a display, on the
software renderer: that checks the harness and compares two builds on one
machine, but is no measure of the desktop.

`--compare BASELINE` flags every gate figure more than 20 % worse than the
baseline's, and exits with 3; `bin/benchmark --summary OUTDIR --compare
BASELINE` compares runs already made. `tests/benchmark-baseline.json` holds
the first baseline, offscreen figures from one aarch64 machine, so compare
offscreen runs with it, and keep a desktop baseline of your own.

## Releases

Every release has its own version. Update `VERSION`, `pkgver` in
`pkgbuild/PKGBUILD` and `CHANGELOG.md` (a dated section for the version)
together, and write `docs/releases/v<VERSION>.md` with the changes and
installation notes. Then build, test, commit, and create an annotated tag:

```sh
release_version=$(cat VERSION)
git tag -a "v$release_version" -m "Release v$release_version"
bin/release
```

`bin/release` writes the tagged source archive and the standalone `PKGBUILD`
that downloads it to `dist/v<VERSION>/`, builds the Arch package from that
`PKGBUILD` and archive, fetches the helpers and test fixtures, runs the tests,
and writes `SHA256SUMS`. It needs Python 3.12 or newer and Arch's `makepkg`.
Packages are built for the current machine's architecture; the build
workflow adds the x86_64 package and its checksum to the release when the
tag is pushed. Each package is also published as `parch-writer-<arch>.pkg.tar.zst`,
a name without the version, which the README's install command downloads
from `releases/latest/download/`.

Maintainers can run `bin/release --publish` to push the commit and tag, set
the GitHub description and topics, and publish the release with its assets.
It needs the GitHub CLI, signed in with admin access to the repository.
Before it pushes anything, it checks the release notes, the tag, GitHub
access, and that the push will be accepted. Verified local assets are reused
on retries, and assets already on the release must match them: one that
differs stops the release before the push, so delete it from the release or
restore the uploaded file, then retry.

Contributions use the project's [MIT license](LICENSE).
