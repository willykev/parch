# Contributing to parch

Bug reports and small improvements are welcome. Open an
[issue](https://github.com/willykev/parch/issues) to discuss a larger change first.

## Build and test

On Arch Linux, install `base-devel`, `qt6-base`, `qt6-declarative`,
`qt6-svg`, `qt6-wayland`, `fontconfig`, `hunspell` and `libyaml`. parch also uses
`xdg-desktop-portal` for file dialogs and the `hunspell-en_us` dictionary for
spell checking; the tests bring a small dictionary of their own.

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

`bin/benchmark OUTDIR` measures launch, typing, scrolling, re-wrapping, idle
time and memory in a real window on three generated documents (10,000 words,
100,000 words, and one long paragraph), three runs each. Run it in a desktop
session with the screen awake, and leave its window in front until it ends.

Contributions use the project's [MIT license](LICENSE).
