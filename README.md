# parch

parch is a Markdown writing app for Omarchy. You write plain Markdown with its
punctuation in view, and read it beside the text as a preview with coloured
code, math, Mermaid diagrams and citations. It saves as you type and brings
your text back after a crash. Your writing stays in ordinary files on your
computer, and there is no account.

[Install](#install) · [Guide](docs/guide.md) · [Changelog](CHANGELOG.md) · [Releases](https://github.com/willykev/parch/releases)

![parch in Split view: a weather-station write-up with its Python script, a formula and a table beside their preview, and the library open on the left](screenshots/hero.png)

## Features

- **Preview beside the text.** `Ctrl+Alt+2` shows tables, coloured code,
  math and footnotes beside the Markdown, scrolling with it.
- **Tables to write in.** A table shows as a grid between your paragraphs:
  `Tab` and `Enter` go from cell to cell, its edges add rows and columns, and
  its Markdown stays lined up for you.
- **Mermaid diagrams.** A `mermaid` block becomes a flowchart, sequence
  diagram, pie or Gantt chart, or any of 23 kinds, redrawn as you type, in
  your theme's colours and with no browser.
- **Citations.** Write `[@key]` against a BibTeX or CSL file, and the preview
  sets the citations and the bibliography in APA, Chicago, IEEE, MLA or a
  style of your own.
- **Export and print.** PDF, Word, EPUB, HTML or Markdown, with the PDF's
  pages shown before anything is written.
- **Vim keys.** Normal, Insert and Visual modes, registers, search and `:w`,
  turned on in **Preferences**; `Ctrl` keys keep their meaning.
- **Focus and spelling.** `Ctrl+Shift+D` dims all but the sentence you are on,
  and misspelled words are underlined as you write.
- **Never lose a word.** parch saves a moment after you pause, keeps unsaved
  text through a crash, keeps earlier versions to go back to, and shows both
  sides when a file changes on disk.
- **Your library.** Folders, drafts and recent files beside the text;
  `Ctrl+Shift+O` finds any document by name.

## Install

parch runs on Omarchy and other Arch Linux systems, on x86_64 and ARM64.

```sh
release=https://github.com/willykev/parch/releases/latest/download
arch=$(uname -m)
sums=SHA256SUMS-$arch
curl -fLO "$release/parch-writer-$arch.pkg.tar.zst" &&
  curl -fLO "$release/$sums" &&
  sha256sum -c --ignore-missing "$sums" &&
  sudo pacman -U "parch-writer-$arch.pkg.tar.zst"
```

For ARM64 releases 0.1.0 and 0.1.1, set `sums=SHA256SUMS`.

Then open **parch** from the app launcher, or run `parch` with the files to
open. The package is `parch-writer`; the command is `parch`.

To build from source instead:

```sh
sudo pacman -S --needed base-devel git qt6-base qt6-declarative qt6-svg qt6-wayland fontconfig hunspell libyaml syntax-highlighting librsvg cairo libzip
git clone https://github.com/willykev/parch.git
cd parch
bin/install
```

Update by running the install commands again, or `git pull` and
`bin/install` in your clone. Remove parch with `sudo pacman -R parch-writer`; your
documents stay where they are.

## Start writing

Press `Ctrl+N` and type. The draft is kept from the first key you press;
`Ctrl+S` gives it a name and a folder, and from then on parch saves it a moment
after you pause. `Ctrl+Alt+2` puts the preview beside it.

### Draw diagrams as you type

Write a `mermaid` block, and the preview beside it draws the diagram, and
draws it again with each line you add.

![A flowchart typed a line at a time in the Markdown on the left, and drawn again in the preview on the right after each line](screenshots/diagram.gif)

Pie charts, sequence diagrams, Gantt charts and the rest of Mermaid's kinds
are drawn the same way, in your theme's colours.

![A pie chart of what the weather station cost: its Mermaid block on the left and the chart on the right](screenshots/pie-chart.png)

### Write in a table

`Ctrl+T` asks how many columns and rows, and puts in a grid with the caret in
its first cell. `Tab` and `Enter` go from cell to cell and add a row at the
end; the handle beside a row or above a column opens its menu or drags it to
another place, and the **+** at an edge adds one. The Markdown underneath is
written again, lined up, with each change.

![The write-up's table of readings as a grid in the text, the caret in a cell and the handles of its row and column showing](screenshots/tables.png)

### See code and math as they read

Fenced code is coloured by its language, and `$…$` and `$$…$$` become
formulas, in your theme's colours.

![The shell and Python blocks of the write-up, plain in the Markdown on the left and coloured in the preview on the right](screenshots/code.png)

### Cite your sources

Name a bibliography and a style in the front matter and write `[@key]` where
you cite: the preview sets each citation, and the bibliography after the text.

![The preview in Reading view: a paragraph citing three works in Chicago author-date style, its footnote, and the bibliography it produces](screenshots/citations.png)

### Export and print

`Ctrl+Shift+E` exports to PDF, Word, EPUB, HTML or Markdown. The dialog says
what the format leaves out of this document and shows a PDF's pages as you
change its options. `Ctrl+P` prints the same pages.

![The Export dialog with PDF chosen: its options on the left, what a PDF leaves out below them, and the first two pages on the right](screenshots/export.png)

### Follow your theme

parch takes its colours and font from your Omarchy theme, light or dark, and
changes when the theme does.

![The same view in Catppuccin Latte and in Gruvbox, side by side](screenshots/themes.png)

### Keys worth knowing

| Key | Does |
| --- | --- |
| `Ctrl+N` | New draft |
| `Ctrl+O` / `Ctrl+Shift+O` | Open a file / find a document by name |
| `Ctrl+S` | Save now |
| `Ctrl+Alt+1` / `2` / `3` | The text / both / the preview |
| `Ctrl+T` | A table, to write in as a grid |
| `F9` | Show or hide the library |
| `Ctrl+F` / `Ctrl+H` | Find / find and replace |
| `Ctrl+Shift+F` | Search the library |
| `Ctrl+Shift+D` | Focus mode |
| `F11` | Distraction-free writing |
| `Ctrl+Shift+E` / `Ctrl+P` | Export / print |
| `Ctrl+Shift+P` | Find and run any command |
| `F1` | The user guide |

**Keyboard shortcuts…** in the menu (`F10`) lists every key and lets you
change them; the [guide](docs/guide.md#keyboard-shortcuts) has them all.

## Use the terminal

Check, render and export documents from a script, with no window.

```sh
parch notes.md                          # open it, in the running parch if there is one
parch check notes.md                    # broken links, missing pictures, unknown citations
parch stats notes.md                    # words, characters and reading time
parch render notes.md --to outline      # the headings; also html, text or the parsed tree
parch export notes.md -o notes.pdf      # also .html, .docx, .epub, or .md with its includes
parch skill install                     # teach your coding agents to use parch
```

Add `--json` for scripts. `parch --help` lists everything, and
`parch help COMMAND` one command's options.

## Performance

- Opens a 10,000-word document ready to type in 0.31 seconds.
- A keystroke shows on screen within 4 ms in that document, and within 11 ms
  in a 100,000-word book with its preview beside it.
- Uses 108 MiB of memory with that document open, 118 MiB with its preview.
- Left alone, it draws nothing once the caret stops blinking, and uses about
  30 ms of processor time a minute.

One paragraph of 10,000 words is the slow case: there a keystroke takes about
60 ms to show.

Measured on an Apple M3 Max running Asahi Linux, Hyprland 0.56.2 and Qt 6.12
at 120 Hz, parch 0.1.0, with `bin/benchmark`: medians of three warm starts,
key times at the 95th percentile, spelling and live formatting on.

## Your data

Your documents are ordinary Markdown files, saved where you put them. parch
keeps drafts and history in `~/.local/share/parch/`, unsaved text and open
tabs in `~/.local/state/parch/`, and settings in `~/.config/parch/`; back
those up with your documents. There is no account and no cloud. The network
is used only for pictures a document names by web address, once you choose
**Show them** above its preview or turn on **Fetch pictures from the web**
in **Preferences**, and never to reach your own computer or local network.
Spelling is checked in English only.

## More

The [guide](docs/guide.md) covers every key, command and setting, the
Markdown parch reads, history and recovery, and what to do when something goes
wrong. See the [changelog](CHANGELOG.md) for what is new, and
[CONTRIBUTING](CONTRIBUTING.md) to build or change parch.

[MIT license](LICENSE) ·
[![Build](https://github.com/willykev/parch/actions/workflows/build.yml/badge.svg)](https://github.com/willykev/parch/actions/workflows/build.yml)
