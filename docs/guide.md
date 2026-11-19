# parch guide

parch is a Markdown writing app for Omarchy. It edits Markdown as plain text,
with every `#`, `*` and `[link](…)` in view, and keeps what you write safe
while you work.

This guide is also in parch: **User guide** in the menu, or `F1`, opens it
in a read-only tab. The package installs it as
`/usr/share/doc/parch/guide.md`. Beside it in the menu, **Markdown guide**
teaches the Markdown parch reads, with examples you can edit; see
[Learn the Markdown parch reads](#learn-the-markdown-parch-reads).

**Contents:** [Install](#install) · [First steps](#first-steps) ·
[Start and open documents](#start-and-open-documents) ·
[Format your writing](#format-your-writing) ·
[See formatting as you write](#see-formatting-as-you-write) ·
[Paste text, images and files](#paste-text-images-and-files) ·
[Learn the Markdown parch reads](#learn-the-markdown-parch-reads) ·
[Preview your writing](#preview-your-writing) ·
[Write math and diagrams](#write-math-and-diagrams) ·
[Citations and bibliographies](#citations-and-bibliographies) ·
[Includes](#includes) · [Export and print](#export-and-print) · [Check a document](#check-a-document) ·
[Find text](#find-text) · [Keep a library](#keep-a-library) ·
[Work in tabs and windows](#work-in-tabs-and-windows) ·
[Check spelling and focus](#check-spelling-and-focus) ·
[Edit with Vim keys](#edit-with-vim-keys) ·
[Keep your writing safe](#keep-your-writing-safe) ·
[Choose how a document is read](#choose-how-a-document-is-read) ·
[Keyboard shortcuts](#keyboard-shortcuts) · [Command line](#command-line) ·
[Settings](#settings) · [Appearance](#appearance) · [Your data](#your-data) ·
[Troubleshooting](#troubleshooting) · [Conformance](#conformance) ·
[What parch does not do](#what-parch-does-not-do) · [Getting help](#getting-help)

## Install

parch runs on Omarchy and other Arch Linux systems, on x86_64 and ARM64. It
is installed from its GitHub releases. The package is called `parch-writer`
(see [Another package called parch](#another-package-called-parch)); the
command is `parch`.

### From a package

Each release has a package for each architecture, under a name that stays
the same from one release to the next:

```sh
curl -LO "https://github.com/willykev/parch/releases/latest/download/parch-writer-$(uname -m).pkg.tar.zst"
sudo pacman -U "parch-writer-$(uname -m).pkg.tar.zst"
```

pacman installs what parch needs from the Arch repositories with it. Then
open **parch** from the app launcher, or run `parch` with the files to open.
It is offered for Markdown files too, so a file manager can open `.md` files
in it.

### From source

```sh
sudo pacman -S --needed base-devel git qt6-base qt6-declarative qt6-svg qt6-wayland fontconfig hunspell libyaml syntax-highlighting librsvg cairo libzip
git clone https://github.com/willykev/parch.git
cd parch
bin/install
```

`bin/install` builds parch, then builds and installs its Arch package with
`makepkg`, asking for any package it still needs, such as `hunspell-en_us`
and `xdg-desktop-portal`. The package holds the program, the math and
diagram helpers (in `/usr/lib/parch`), their licences, the launcher entry and
icon, this guide in `/usr/share/doc/parch`, and an example write-up with its
bibliography in `/usr/share/doc/parch/examples/weather-station`.

### Another package called parch

The AUR has an unrelated package named `parch`: a wrapper for paru and
pacman, which installs its own `/usr/bin/parch`. This parch's package is
named `parch-writer` so that AUR helpers such as `yay` never mistake one for
the other. The two cannot be installed together, since both have a `parch`
command: installing one asks to remove the other.

### Update

Run the two commands under [From a package](#from-a-package) again. Built
from source, run `git pull` and then `bin/install` in your clone. Your
documents, drafts, history and settings are kept.

### Remove

```sh
sudo pacman -R parch-writer
```

Your documents stay where they are. parch's own folders stay too (see
[Your data](#your-data)); to remove them as well:

```sh
rm -rf ~/.local/share/parch ~/.local/state/parch ~/.config/parch ~/.cache/parch
```

That deletes your drafts and version history, so save any draft you want to
keep as a file first.

### What it changes on your system

Nothing outside its own files and folders: parch never edits your desktop's
configuration or theme. The package adds a launcher entry, which offers
parch for Markdown files, and removing the package takes it away.
`parch skill install` adds a skill for coding agents in your home folder (see
[For coding agents](#for-coding-agents)).

## First steps

1. Open **parch** from the app launcher, or run `parch` in a terminal. The
   window opens with an empty draft, and the library beside it.
2. Type. A draft is kept from the first key you press, so nothing is lost if
   you close the window; it is listed under **Drafts** in the library.
3. Press `Ctrl+S`, choose a folder and a name, and the draft becomes a file.
   From then on parch saves it a moment after you pause.
4. Press `Ctrl+Alt+2` to see the preview beside the text, and `Ctrl+Alt+1`
   for the text alone.
5. In the library (`F9`), **Open folder** adds the folder you keep your
   writing in, so that its documents are a click away and `Ctrl+Shift+O`
   finds them by name.
6. `Ctrl+Shift+P` finds any command by name, and `F10` opens the menu.
   **Markdown guide** in the menu teaches the Markdown parch reads.

To try parch on something written already, copy the example the package
installs, a short write-up with code, math, a table, a diagram and
citations, and open it:

```sh
cp -r /usr/share/doc/parch/examples/weather-station ~/
parch ~/weather-station/weather-station.md
```

## Start and open documents

- **New draft** (`Ctrl+N`) opens an empty tab. A draft is kept in parch's own
  folder from the first keystroke, so it survives closing the window and
  restarting. It is listed under **Drafts** in the library.
- **Save** (`Ctrl+S`) on a draft asks where to put it, suggesting a name from
  the visible text of its first heading or its first line. From then on it
  is an ordinary file.
- **Open** (`Ctrl+O`) uses your desktop's file dialog. Opening a file that is
  already open shows its tab instead of opening it twice.
- Opening a document replaces an empty draft you have not typed in.
- Run `parch new-note.md` with a name that does not exist yet to start a
  document that becomes that file when you first type.
- A file that is not valid UTF-8 opens read-only, so its bytes stay exactly as
  they are. **Reopen as…**, in the bar above the text, the menu or the
  command palette, reads it in the
  encoding it was written in: Windows-1252, Latin-1, UTF-16 and every other
  your system knows, with a preview of the text in each. Once you convert
  it, you can edit it, and nothing is saved until you choose **Save as
  UTF-8**, which keeps the file's original bytes in its history first.
- parch opens files of up to 64 MiB, at paths of up to 4,095 bytes with every
  link followed. A larger file, or a longer path, is not opened, and parch
  says why.

parch keeps each file's line endings and byte-order mark. Text you paste gets
the document's line endings.

## Format your writing

Every formatting command writes plain Markdown into the text, and each is one
step for `Ctrl+Z` to take back. Find them under **Format** in the menu
(`F10`), on the toolbar under the tabs, in the command palette, or by their
keys.

- **Bold**, **Italic**, **Strikethrough**, **Highlight** (`==`),
  **Superscript** (`^`), **Subscript** (`~`), **Inline code**, **Inline
  math** (`$`) and **Comment** (`<!-- -->`) wrap the selection, or the text
  of each line in a selection over several lines. Superscript and subscript
  write the delimited form, as in `y^(a+b)^` and `H~2~O`, which holds
  punctuation but no spaces, right after the letter or digit it belongs to.
  Where the preview would not read it so, after a space or another mark of
  Markdown, around a selection with a space in it, or inside code, they
  change nothing and are greyed out. Used again on text that has them, they
  take the markers away. Without a selection they take away the markers
  around the caret, or put a pair there with the caret between them.
- **Heading 1** to **Heading 6** and **Paragraph** change every line the
  selection touches; a heading at the level chosen becomes a paragraph again.
  Headings are marked with `#`, or, if you choose so in **Preferences**,
  levels one and two are underlined with `===` and `---`.
- **Quote**, **Bulleted list**, **Numbered list** and **Task list** mark each
  line, or take the marks away when every line has them. **Check or uncheck
  task** (`Ctrl+L`) ticks the task boxes on the caret's lines.
- **Code block** and **Display math** put the lines between fences, or take
  the fences away.
- **Link** (`Ctrl+K`) and **Image** wrap the selection and select `url` to
  type over. A selected web address becomes the link's destination. Used
  inside a link, **Link** takes the link away and keeps its text.
- **Footnote** inserts the next `[^n]` and its definition at the end of the
  document, where the caret goes for you to write the note.
- A reference such as `[^source]` that no `[^source]:` defines leads
  nowhere. A right click on it, or the `Menu` key with the caret on it,
  offers **Turn into an inline note**, which makes it `[^source.]`, a note of
  its own, and **Add a definition for the note**, which puts `[^source]: `
  at the end of the document for you to write the note. Both are in the
  command palette while the caret is on such a reference.
- **Table** inserts a two-column table; **Format table** lines up a table's
  pipes and padding by its alignment row without changing a cell, and keeps
  wide characters, escaped pipes and pipes in code in their place.
- **Thematic break** and **Page break** (`+++`) go on a line of their own,
  with a blank line before them.

While you type:

- `Enter` in a list item starts the next item: the same bullet, the next
  number, or an unchecked box. `Enter` on an empty item ends the list. A
  list numbered `1.`, `1.`, `1.` goes on with `1.`; in a list that counts
  up, a new item in the middle renumbers the items after it, in the same
  undo step. **Renumber list** in the Format menu counts the numbered list
  at the caret up from its first number.
- `Tab` and `Shift+Tab` indent and outdent a list item, or every line of a
  selection; elsewhere `Tab` types a tab.
- In a table, `Tab` and `Shift+Tab` go to the next and previous cell, and
  `Tab` in the last cell adds a row.
- `Shift+Enter` ends the line with a Markdown hard line break.

The status line counts the words of the document, or of the selection, and
how many minutes they take to read at 200 words a minute. It counts what a
reader sees: link addresses, task boxes, comments, front matter and
Markdown punctuation are not words. Chinese and Japanese count each
character as a word.

## See formatting as you write

parch styles your Markdown as you write and never hides any of it: every `#`,
`*` and `[link](…)` stays in the text, drawn dimmed, while what it marks takes
its look. Headings are in the heading weight, bold text is bold and italic
text italic, struck-through text is struck through, and highlighted text
(`==`) has the highlighter's colour behind it. The text of links and images
is in the theme's accent with the address dimmed, and so are notes,
citations and `[%variables]`. Code is in the theme's code colour, a code span
on a tinted ground; math between dollars is tinted. List and quote marks,
task boxes, table pipes, code fences, page breaks (`+++`) and thematic breaks
are dimmed, and so are comments, front matter and link definitions.

Styling changes colour, weight and slant only, never the size or the spacing
of the text, so the caret, the selection and where lines break stay exactly
where they are. If the editor's font draws bold or italic wider than its
plain text, as proportional fonts do, that text takes a colour instead. Every
colour comes from your Omarchy theme and changes with it; a theme colour too
pale to read as text on your background, as a yellow often is on a light
one, is drawn closer to the colour of the text.

New formatting is styled as soon as you pause, in a book-length document too,
and text typed at the end of a heading or inside bold text takes its style at
once. **Turn off live formatting**, in the menu (`F10`) or the command
palette, or **Live formatting** in **Preferences…**, shows the source as
plain text; turning it on again styles it at once.

## Paste text, images and files

- **Paste** (`Ctrl+V`) turns rich text copied from a web page or a word
  processor into Markdown: headings, emphasis, links, lists and task lists,
  quotes, code and tables. Scripts, styles and hidden text are left out, and
  characters that would otherwise turn into Markdown are escaped. One undo
  takes the whole paste back. `Ctrl+Shift+V` always pastes plain text.
- A pasted image, such as a screenshot, is saved in the `assets` folder beside
  the document, named after the day as in `pasted-2026-11-16-1.png`, and
  linked where you pasted it. Pasting the same image again reuses its file.
  An image pasted without alt text puts the caret between `![` and `]`, so
  that you can describe it for readers who cannot see it; one undo still
  takes the paste back.
  Images in the web page itself stay links to where they are online; nothing
  is downloaded.
- Dropping files on the text, or pasting files copied in a file manager,
  links them where they land. A file already in the document's folder or in
  the library is linked where it is; anything else is copied into `assets`,
  keeping its name, with `-2`, `-3` added when another file has it. Files
  over 50 MiB are copied only once you agree, and nothing you drop is moved.
  An `assets` that is a link to another folder is never followed: nothing is
  pasted or dropped into it, and a notice says so.
- Dropping a Markdown document offers **Insert link** or **Include**, which
  puts its path on a line of its own so that its text is shown there.
- A draft keeps its assets in its own folder until you save it; they then go
  into `assets` beside the file. An asset whose name another file already
  has there, as a screenshot pasted into another draft on the same day, is
  given `-2`, `-3` before its extension, and the draft's links to it are
  changed to match before the file is written; one undo changes them back.

## Learn the Markdown parch reads

**Markdown guide**, beside **User guide** in the menu (`F10`) or in the
command palette, opens a tab that teaches the Markdown parch reads, beside its preview: every
part of the dialect as an example, and the cases that do not do what you
might expect, such as emphasis that never closes or a list that starts at 7.
What the preview shows is what parch makes of the text, so change an example
and watch it change. Its pictures, its included file and its bibliography
come with parch, so every example works without any file of yours.

Nothing written in the guide is kept. It is not a draft, it is never saved or
kept in recovery or history, the next launch does not bring it back, and
closing it asks nothing. Opened again while it is open, it comes to the front
as it is; once closed, it opens fresh. **Save** or **Save as…** writes a copy
where you choose, which is then an ordinary document. Its last part tells how
to read it as Standard Markdown in **Document settings…**, a choice that is
not kept either.

## Preview your writing

parch shows what a document reads as, set in a reading font in the colours of
your theme. There are three views, under **View** in the menu:

- **Source view** (`Ctrl+Alt+1`): the text alone.
- **Split view** (`Ctrl+Alt+2`): the text and its preview side by side.
- **Reading view** (`Ctrl+Alt+3`): the preview alone, in a column in the
  middle of the window.

`Ctrl+Alt+P`, or the button at the right of the header, shows the preview
beside the text and hides it again. Each tab keeps its own view, and the next
launch brings it back. A new tab opens beside its preview when the tab in
front is.

In a split, drag the line between the two to give one more room; or reach it
with `Tab` and press `Left` or `Right`, and `Home` to put it back in the
middle. A window too narrow for both shows one at a time: **Markdown** and
**Preview** above the text choose which, as does `Ctrl+Alt+E`.

The preview follows the text a moment after you pause, and while you keep
typing it catches up at least every three quarters of a second, so that it
does not hold up the keys you type. Only what changed is set again,
so it stays where you were reading. While a long document takes more than a
moment to set again, the preview keeps what it showed and says **Updating…**
in its top corner until it has caught up. It follows the files the text uses as well: a picture, an included file, a
bibliography or a `.csl` style changed on disk is shown changed without an
edit, and only what uses it is set again.

The formatting toolbar is hidden in the reading view, where there is no text
to format, and comes back in the source and split views; the room it takes
stays, so the preview does not move.

What it shows:

- Headings, paragraphs, emphasis, strikethrough, highlights, superscripts and
  subscripts, links, quotes, lists, task lists, definition lists, thematic
  breaks, and a quiet mark where a page break (`+++`) is.
- Code blocks coloured by their language, and tables with their alignment and
  merged cells. A table, a code block or a formula wider than the column
  scrolls sideways by itself; the text around it never does.
- Images, scaled to the column. A path is read from the document's folder;
  a web address is fetched, unless you turn that off in **Preferences**.
- Math (`$…$`, `$$…$$`) and Mermaid diagrams, drawn in the theme's colours.
  The source stands in until the picture is drawn, and if it cannot be, the
  reason is shown in its place.
- Footnotes as numbered notes after the text, a table of contents for
  `{{TOC}}`, the text of included files, and `[%key]` values from the front
  matter.
- Citations and the bibliography as the document's citation style writes
  them, with a note style's citations among the notes; see
  [Citations and bibliographies](#citations-and-bibliographies).
- Not comments or front matter. Raw HTML that parch supports is formatted;
  anything else is shown as the text it is and never run (see
  [Raw HTML](#raw-html)).
- What leads nowhere stays as you wrote it, in the theme's danger colour,
  and pointing at it says why: an include that cannot be read in (for a
  cycle, the chain of files that leads back), and a variable, a reference,
  a footnote or a heading link that nothing defines. Assistive technology
  hears the same with the preview's description.

The preview reads the document as its [document settings](#choose-how-a-document-is-read)
say: a document in the Standard profile is previewed as Standard.

In the preview:

- It scrolls with the text beside it, each showing the same place. **Scroll
  sync** in the status line turns that off and on.
- A click on a link opens a web or mail address in your browser or mail
  program and a Markdown file in a tab, at the heading the link names if it
  names one, and scrolls to a heading in the same document. A footnote's
  number goes to the note and the arrow after the note comes back.
- A link of any other kind asks first. An address such as `zoommtg:` or
  `steam:` names the program it would go to by its scheme, and shows the
  whole address; **Open** hands it to that program. A link to any other file
  on your computer, such as a PDF, a script or a folder, never opens from
  the preview: **Show in folder** shows it in your file manager, where you
  can open it yourself. Resting the pointer on a link always shows where it
  leads, with its title, if it has one, after that.
  **Back in the preview** (`Alt+Left`) returns to where you were.
- A click on a task's box ticks it in the text; `Ctrl+Z` takes that back.
- A double click puts the caret at the same place in the text. In text an
  include brought in, it opens the included file in a tab, with the caret
  at the place you clicked.
- Select text with the mouse, or with `Shift` and the arrow keys, and copy it
  with `Ctrl+C`, as plain text and with its formatting.
- `Up`, `Down`, `PageUp`, `PageDown`, `Space`, `Home` and `End` scroll.
  `F6` moves the keys between the library, the text and the preview.

## Write math and diagrams

parch draws formulas and diagrams itself, with two programs that come with
it: RaTeX for TeX math and Merman for Mermaid diagrams. Both run on your
computer, cut off from the network, and only while they draw. What they
cannot draw keeps its source in place, with the reason (**Check document**
and `parch check` list them too).

### Math

Formulas are written in the LaTeX that KaTeX 0.16 supports: the commands on
KaTeX's list of supported functions, from accents, delimiters and
environments (`matrix`, `cases`, `aligned`, `align`, `gather`, `CD` and the
rest) to operators, relations, arrows, fonts, colours, sizes, spacing and
macros (`\def`, `\newcommand`). parch is tested against a corpus with one
expression for each command on that list, 1,043 in all, drawn with RaTeX
0.1.15, the version parch comes with: 1,037 of its 1,043 expressions draw.

These do not draw: `\left` and `\right` with `\llbracket`, `\rrbracket`,
`\lBrace` and `\rBrace` (the four draw at their own size, without
`\left`); `\includegraphics`; and `\expandafter`.

Other differences from KaTeX:

- Right-to-left text in a formula, such as Hebrew or Arabic in `\text{…}`,
  comes out with its letters in reverse order.
- `\href` and `\url` draw their text, but a formula is a picture, so it is
  not a link. `\htmlId`, `\htmlClass`, `\htmlStyle` and `\htmlData` draw
  what they hold and nothing more.
- A formula is a picture of the formula: its text cannot be selected, and
  assistive technology reads its TeX.

### Diagrams

A fenced block marked `mermaid` is a diagram. Diagrams are written in
Mermaid 12.3 syntax and drawn through Merman 0.10, the version parch comes
with, in the colours of your theme. These 23 families are drawn and tested,
each a block that starts with its keyword:

| Family | Keyword |
| --- | --- |
| Flowchart | `flowchart` or `graph` |
| Sequence diagram | `sequenceDiagram` |
| Class diagram | `classDiagram` or `classDiagram-v2` |
| State diagram | `stateDiagram` or `stateDiagram-v2` |
| Entity relationship diagram | `erDiagram` |
| User journey | `journey` |
| Gantt chart | `gantt` |
| Pie chart | `pie` |
| Quadrant chart | `quadrantChart` |
| Requirement diagram | `requirementDiagram` |
| Git graph | `gitGraph` |
| C4 diagram | `C4Context`, `C4Container`, `C4Component`, `C4Dynamic` or `C4Deployment` |
| Mind map | `mindmap` |
| Timeline | `timeline` |
| Sankey diagram | `sankey-beta` |
| XY chart | `xychart-beta` |
| Block diagram | `block-beta` |
| Packet diagram | `packet-beta` |
| Kanban board | `kanban` |
| Architecture diagram | `architecture-beta` |
| Radar chart | `radar-beta` |
| Treemap | `treemap-beta` |
| ZenUML | `zenuml` |

A diagram's `accTitle:` and `accDescr:` are what assistive technology
hears; without them it hears the kind of diagram and its words. Where
Merman differs from Mermaid: a C4 diagram's relation labels sit on their
lines, a label such as `"A & <B>"` loses what follows the `&`, Markdown in
a label (`"**Bold** label"`) keeps its asterisks, and the colours of a
Sankey diagram's nodes and of architecture icons are Mermaid's own rather
than the theme's. A diagram is a picture: its text cannot be selected.

## Citations and bibliographies

Cite a work in either of two ways, which can be mixed:

```markdown
A statement[p. 23][#Doe:2006].          Writer: the locator, then the key
Another statement [@doe2006, p. 23].    Pandoc: the key, then the locator
Doe shows [-@doe2006] that it works.    Pandoc, leaving the author out
Two at once [@doe2006, p. 5; @roe2010].
More [see @doe2006, ch. 2; also @roe2010].   Words before each key
@doe2006 [p. 4] shows it as well.       The author named in the sentence

[#Doe:2006]: John Doe. *A Sample Book*. Example Press, 2006.
```

In brackets, a key's `@` starts the item or follows a space, so an e-mail
address is never a citation, and brackets followed by `(` or `[` are a link.
Without brackets, `@key` is a citation only when the document or its
bibliography has that key, so a handle such as `@someone` stays text.

A Writer definition, `[#key]: …`, is a bibliography entry written in the
document itself. Keys of a bibliography file are cited the same way: choose
the file under **Bibliography** in **Document settings…**, or name it in the
front matter, with the style:

```yaml
---
bibliography: references.bib      # or a list: [books.bib, papers.json]
csl: chicago-author-date           # a bundled style, or a .csl file
link-citations: true               # each citation links to its entry
---
```

parch reads BibTeX and BibLaTeX (`.bib`), CSL JSON (`.json`) and CSL YAML
(`.yaml`, `.yml`) files, found from the document's folder when the front
matter names them, and reads a file again when it changes. The bundled styles
are APA (`apa`), Chicago author-date (`chicago-author-date`), Chicago notes
and bibliography (`chicago-notes-bibliography`), IEEE (`ieee`), MLA
(`modern-language-association`, or `mla`), Cite Them Right Harvard
(`harvard-cite-them-right`, or `harvard`) and NLM/Vancouver
(`nlm-citation-sequence`, or `vancouver`); a `.csl` style file of your own
works as well. A chapter you include can name a bibliography of its own in
its front matter; one that does not uses the document's.

A front matter names files only within the document's library, or, for a
document in no library, within its folder and the folders in it, as
includes do (see [Includes](#includes)). A bibliography or style it names
elsewhere, `~/refs.bib` or `../../other.bib` say, or through a link that
leads out, is not read, and the preview and `parch check` say why. So a
document from someone else cannot read your other files. A file you choose
yourself is read wherever it is: under **Bibliography** and **Citation
style** in **Document settings…**, or with `--bibliography` and `--csl`.
Whatever the file, parch reads it only if it is a regular file of at most
32 MiB, never a device, a pipe or a file of `/proc`, `/sys` or `/dev`.

With a style, the preview, `parch render` and the HTML export write each
citation as the style has it, `(Doe, 2006, p. 23)` or `[1, p. 23]`, and the
bibliography after the text: the style's entries, then the document's own
Writer entries, numbered as they are first cited. A note style, such as
Chicago notes and bibliography, puts each citation of the text in a footnote
of its own, numbered with your other notes; a citation inside a footnote
stays in it. Without a style or a bibliography file, Writer citations are
numbered as they are first cited: `[1, p. 23]`.

In the preview a key that nothing defines stays where it is cited, in the
theme's colour for what is wrong, and says which key it is when the pointer
rests on it. The preview shows a new style or bibliography as soon as you
choose it, and a bibliography or style file changed on disk as soon as it
changes.

`parch check` reports a key that neither the document nor its bibliography
defines, a key given to two entries of one file or found in two files, a
bibliography that cannot be read or has errors, with their lines, a style
that cannot be used, and citations of a bibliography with no style chosen.

What citations cannot do yet:

- Only English is bundled: US English, and British English for the styles
  written for it. A style written for another language is formatted with
  US English words and dates, and `parch check` says so.
- Styles are CSL 1.0.2. A CSL-M style, the legal extension of CSL, is
  refused with the element it uses that CSL does not define.
- parch formats citations with its own CSL processor, checked against the
  CSL test suite's English fixtures: 746 of the 809 pass. Most of the rest
  are rare cases of telling authors and years apart, of "ibid." across
  notes and the text, and of capitalisation.

## Includes

A line that names another Markdown file, `chapters/one.md`, alone in its
paragraph, puts that file's text in its place, in the preview, `parch check`,
`parch render` and every export. A path that starts with `/` is found in the
library root; any other beside the file that names it.
Put a path with spaces or Markdown punctuation such as brackets in single
or double quotes. Within that path, escape the matching quote or a literal
backslash with a backslash.

parch reads an include only from within the document's library, or, for a
document in no library, from its folder and the folders in it. An include
that leads out of them, through `..`, a `/../` path or a link to a file
elsewhere, is not read: the preview marks the line, and pointing at it says
it is outside the library (or the document's folder); `parch check` reports
it as `include-outside`, and the document is not exported until it is
fixed. A link that stays inside, such as a chapter linked from another folder
of the library, is read as usual. Only a regular file of at most 64 MiB is
read, never a device, a pipe or a file of `/proc`, `/sys` or `/dev`. So a
document from someone else shows and exports its own files and none of
yours.

## Export and print

**Export…** (`Ctrl+Shift+E`) writes the document in front as Markdown, HTML,
PDF, DOCX (Word) or EPUB (an e-book). The dialog shows the format's options
and, before anything is written, what that format leaves out of this
document: that formulas and diagrams become pictures in a Word document, for
example, or that pictures from the web are not fetched for a PDF. **Export…**
then asks your desktop's file dialog where to write it. A name given without
the format's extension gets it, and if a file of that name with the
extension is there already, parch asks before replacing it.

For a PDF, and for printing, the dialog also shows the pages as they will
come out, laid out again as you change the paper, the margins, the type or
the template. Tab reaches them, and the arrow keys, Home and End move from
page to page.

- The export reads the document as it is when it starts, with the files it
  includes, its bibliography and its citation style, in its own
  [document settings](#choose-how-a-document-is-read), and you can go on writing
  meanwhile. Its progress shows above the text, with **Cancel**. When it
  ends, the notice says what was written and what was left out, or why
  nothing was.
- An export protects the document and its included files. If an export
  fails or you cancel it, an existing output keeps its previous contents.
- When exporting again, parch reuses identical pictures in the folder it
  made beside the output. If a name is taken by different content, the new
  picture gets a number such as `figure-2.png`. After the export succeeds,
  parch removes unused copies it can still identify as its own. It keeps
  files you added or edited, pictures shared by exports, and older copies
  whose ownership the filesystem cannot verify. To publish just the current
  pictures, export into an empty folder.
- An export never writes through a link. When the file it would write is a
  symbolic link, parch says where the link leads and asks: **Export as…**
  chooses another file, and **Replace the link** puts the export in place
  of the link itself, leaving the file it led to as it was. A folder of
  pictures that is a link is never replaced.
- **Export again** repeats the document's last export: the same format and
  options, to the same file, with the text as it is now.
- **Print…** (`Ctrl+P`) prints the document as the PDF export sets it,
  through your desktop's print dialog, which chooses the printer and the
  paper; the dialog before it chooses the margins and the type.

What each format holds:

- **Markdown** assembles the document and its includes into one file. The
  default image setting, **auto**, links pictures within the new document’s
  folder and copies other readable pictures into `NAME_files` beside it.
  Choose **copy** to carry all its pictures, **link** to point back to the
  originals, or **Write the text as it is** to keep the source text.
  Assembled Markdown carries the effective bibliography and style when the
  text uses citations. It omits citation files the source's front matter
  names outside its allowed folders and reports them as omitted.
- **HTML**: a page of its own, in typography that does not follow the
  desktop theme. Images are copied into a folder beside it (`NAME_files`),
  written into the page, or linked where they are. If an allowed picture is
  missing or in a format parch cannot copy, HTML links to its original
  location and the export report names it. Formulas are SVG, named by
  their TeX for screen readers, and diagrams are SVG pictures described by
  their words.
- **PDF**: pages of the size and margins you choose (A4 and 20 mm unless you
  choose others), in a template: **classic**, a face made for reading at 11
  points; **modern**, the desktop's sans-serif at 10 points; or **compact**,
  10 points with close lines and 15 mm margins. A font, a size or margins
  you choose go over the template's. The pages are numbered, with text you
  can search and copy, working links, and an outline of the headings. `+++` starts a new page, and a heading never stays
  at the foot of a page without its text. Formulas and diagrams are vectors;
  a diagram that cannot be drawn as one is printed as a picture at four times
  its size, and the notice says so. Notes and the bibliography follow the
  text.
- **DOCX**: a Word document with real styles (Title, Heading 1 to 6, Quote,
  Code), bulleted, numbered and task lists, tables with merged cells, links,
  notes as footnotes or endnotes, pictures with their descriptions, and the
  title, author, subject, keywords and language from the front matter.
  Formulas and diagrams are pictures, not Word equations.
- **EPUB**: an EPUB 3 book with a file for each chapter (each heading of
  level 1, or of level 2 when the document has a single title), a table of
  contents from the headings, its images, formulas and diagrams as SVG, and
  its title, author, language, description and date from the front matter.
  Page breaks are hints, since a reader sets its own pages, and pictures from
  the web are left out with their descriptions in their place.

Formulas and diagrams are drawn by the same helpers as the preview, dark on
white whatever the desktop's theme. Without them they are written as their
source, and the dialog says so before you export.

## Check a document

**Check document…** in the menu or the command palette lists everything
wrong in the document and the files it includes, a line each as
`file:line:column: message`: links, images and includes whose files are not
there, links to headings (`#a-heading`, `other.md#a-heading`) that the
document, with what it includes, or that other document does not have,
references, notes, citations and variables that lead nowhere, include
cycles (with the chain of files) and limits, malformed table rows,
unclosed math, formulas and diagrams that cannot be drawn, citation and
bibliography problems, and HTML shown as text. It reads the text as it is
when you ask, without holding up the window. `Up` and `Down` move through
the list, and `Enter` opens the place, in the included file when the
problem is in one.

`parch check` reports the same problems from the terminal, and can check
several documents at once (see [Command line](#command-line)).

## Find text

- **Find…** (`Ctrl+F`) opens a bar above the text. `Enter` and `F3` go to the
  next match, `Shift+Enter` and `Shift+F3` to the one before, and the bar
  counts them ("3 of 12"). The bar's buttons match case, whole words, or a
  regular expression.
- **Find and replace…** (`Ctrl+H`) adds a replace field: **Replace** changes
  the current match and **All** every match, as one step for
  `Ctrl+Z`. With regular expressions, `$1` in the replacement is the first
  group.
- `Escape` closes the bar and gives the keys back to the text.
- **Search the library…** (`Ctrl+Shift+F`) searches the text of every
  document in the library's folders and the drafts. Matches arrive as they
  are found, grouped by document, with the line around each; choosing one
  opens the document there.
- **Go to heading…** (`Ctrl+R`) lists the document's headings to jump to.
  Searching shows the parent headings of each match. The focused editor uses
  this key for redo in Vim Normal or Visual mode.

Each document opens again where you were in it last time.

## Keep a library

`F9` shows or hides the library beside the text. Drag its edge to make it
wider or narrower, or reach the edge with `Tab` and use `Left` and `Right`
(`Home` and `End` for as narrow or as wide as it goes). Each window keeps its
own width, and the session brings it back; a new window starts at the width
you chose last.

- **Drafts** lists your drafts. **Recent** lists the files you opened last.
- `Delete` on a recent file, or **Remove from recent** in its menu, takes it
  out of **Recent**; the file stays where it is. **Clear recent files**, in
  the menu (`F10`), the command palette and the menu of **Recent**, empties
  the list, and **Undo** in the notice that says so brings it back.
- `Delete` on a draft, or **Delete draft** in its menu, deletes it. Its text
  is kept in its history first, and **Undo** in the notice that says so
  brings the draft back, attachments and all, while the notice shows. A draft
  open in another window with text not yet saved is not deleted.
- **Open folder** adds a folder. Its `.md`, `.markdown` and `.txt` files are
  listed beneath it and follow changes on disk.
- **Find a document** (`Ctrl+Shift+O`) filters every library folder by name,
  including folders you have not opened.
- **Outline**, at the foot of the library, lists the headings of the document
  in front; choosing one goes there, in the text and in the preview. The
  headings of the files it includes are listed where it includes them, and
  choosing one of those opens its file at the heading, as **Go to heading**
  does. `Ctrl+Alt+O` shows it with the keys on the headings, and hides it.
- A folder's menu (right click, or the `Menu` key) makes a **New document…**
  or **New folder…** in it. A document's menu can **Rename…** it (`F2`),
  **Move to…** another library folder (or drag it there), and **Move to
  trash…** (`Delete`), from where the desktop's trash can bring it back.
  The same commands are in the menu (`F10`) and the command palette for the
  document in front, with **New document in folder…**, which makes one
  beside it, when it is in one of the library's folders.
- When a renamed or moved document has links, images, includes or reference
  definitions leading to it in other documents of the library, or relative
  ones of its own, parch lists them and offers to update them. Open documents are updated in their tabs, as one step for `Ctrl+Z`.
  A moved document keeps its tab, its history and any unsaved text.
- **Undo**, in the notice a rename or move leaves, puts it back where it
  was, and the links it updated with it, as long as nothing else has taken
  its old name and no other rename or move has been made since.

## Work in tabs and windows

- `Ctrl+Tab` / `Ctrl+Shift+Tab`, or `Ctrl+PageDown` / `Ctrl+PageUp`, move
  between tabs. `Ctrl+W` closes one.
- `Ctrl+Shift+N` opens a new window. **New view** in the menu shows the same
  document in another tab; edits appear in both.
- Closing a tab saves it first. If it cannot be saved, parch asks before
  closing.
- `Ctrl+Q` saves everything and quits. The next launch reopens your windows
  and tabs where you left them.

## Check spelling and focus

parch checks English spelling as you write, once a pause lets it, and
underlines misspelled words with a wavy line. Code, URLs and email addresses,
math, HTML, front matter and link destinations are not checked. Right-click a
misspelled word, or press the `Menu` key or `Shift+F10` with the caret on it,
for up to five suggestions, **Add to dictionary** (kept for good) and
**Ignore** (until parch ends). **Check spelling**, in the menu or the command
palette, or **Preferences…**, turns spell checking on or off. It needs the
`hunspell-en_us` dictionary; without it, spelling is off and parch says so
when you turn it on.

**Focus mode** (`Ctrl+Shift+D`) dims everything but the sentence you are
writing; **Preferences…** switches it to the paragraph. It dims the text you
write, never its preview. Dimming blends the text's colour into the
background: **Strong**, the default, or **Light**, and in any theme never so
far that the dimmed text reads at less than 3:1 against the background.
**Off**, under **Dimming** in **Preferences…**, leaves the text as it is
while focus mode stays on. **Distraction-free writing** (`F11`) hides the
library, the tabs, the header, the toolbar and the status line, and leaves
only the document, in the view it is in: the text, centred; the text and its
preview side by side; or the preview alone. `F11` or `Esc` brings them back,
and so does asking for the library or the toolbar.

## Edit with Vim keys

Vim keys are off until you turn them on: **Turn on Vim keys** in the menu
(`F10`) or the command palette, or **Edit with Vim keys** in **Preferences**.
The editor then starts in Normal mode, the caret is a block on the character
it is on, and the status line shows the mode (NORMAL, INSERT, VISUAL or VISUAL
LINE) with, beside it, the command line as you type it, the keys of a command
not yet complete, or what the last command had to say. Vim keys are keys of
the text: the preview keeps its own, beside the text or alone, and reading
view shows no mode.

**Leaving them.** `Esc` (or `Ctrl+[`) always returns to Normal mode, from
Insert or Visual mode, the command line or a command half typed. The command
palette (`Ctrl+Shift+P`) and the menu (`F10`) work in every mode, and
**Turn off Vim keys** in either, or the box in **Preferences**, turns them off
at once. The usual shortcuts such as `Ctrl+S` and `Ctrl+Z` still save and
undo. `Ctrl+R` redoes a change in the focused, editable Vim editor in Normal
or Visual mode. With a command partly typed, it cancels the command. In a
read-only document, in Insert mode or with a Vim command line open, `Ctrl+R`
opens Go to heading. A modifier key by itself leaves a partly typed command
pending.

### Normal mode

A count before a command repeats it or goes that far, up to 9999: `3w`,
`2dd`, `d3w`, `2d3w`, `5G`. These keys are covered:

- **Moving:** `h` `j` `k` `l` and the arrow keys; `Backspace` and `Space`,
  which go on to the line before or after; `Enter`, to the first non-blank
  character of the next line; `w` `b` `e` and `W` `B` `E`, by words and by runs of
  everything but blanks; `0` (or `Home`), `^` and `$` (or `End`); `gg` and `G`,
  to the first and last line or to the line counted; `{` and `}`, to the empty
  line before or after; `%`, to the bracket that matches the one at or after
  the cursor, or with a count that far through the document, in percent; `f`
  `t` `F` `T` and a character, repeated with `;` and `,`; `/` `?` `n` `N` `*`
  `#`, under Search below. `j` and `k` keep their column and move by lines of
  the text, not by lines wrapped on screen.
- **Operators** `d` (delete), `c` (change), `y` (yank), `>` and `<` (indent
  and outdent, as `Tab` does: list items nest, other lines take a tab). Each
  takes any motion above, its own key again for whole lines (`dd`, `cc`,
  `yy`, `>>`, `<<`), or a text object: `iw` `aw` and `iW` `aW` (a word or
  WORD, `a` with its blanks); `ip` `ap` (a paragraph, `a` with the blank
  lines after it); `i"` `a"`, `i'` `a'` and `` i` `` `` a` `` (quoted text on
  the line); `i(` `a(` (also `i)` `ib`), `i[` `a[` (also `i]`) and `i{` `a{`
  (also `i}` `iB`), a bracketed block, the `a` forms with their brackets. `cw`
  on a word changes to its end, as in Vim.
- **Commands:** `x` (or `Delete`) and `X` delete characters; `s` and `S`
  change characters and lines; `C` and `D` change and delete to the end of
  the line; `Y` yanks lines; `J` joins lines with one space; `r` and a
  character replaces characters (`r` `Enter` breaks the line); `~` switches
  case; `p` and `P` put after and before; `u` and `Ctrl+R` undo and redo; `.`
  repeats the last change, with a new count if you give one.
- **Typing:** `i` (or `Insert`) and `a` insert before and after the cursor,
  `I` and `A` at the start and end of the line, and `o` and `O` on a new
  line below and above. A count types what you type that many times: `3ix`
  then `Esc` types `xxx`.
- `v` and `V` start Visual mode; `:`, `/` and `?` the command line.

Keys that are no command here do nothing and type nothing.

### Insert mode

Insert mode types exactly as parch does without Vim keys: lists continue on
`Enter`, `Tab` indents, `Shift+Enter` breaks the line, input methods compose,
and paste converts rich text. `Esc` or `Ctrl+[` returns to Normal mode, one
character to the left. Outside Insert mode the editor takes keystrokes
directly. After `f`, `t` or `r`, a dead key leaves the command waiting for
the next printable character.

### Visual mode

`v` selects by character and `V` by whole lines; pressing the same key
again, or `Esc`, leaves, and the other key switches. A selection made with
the mouse, or with `Shift` and the arrows, is a Visual mode selection too.
Every motion and text object above moves the selection's end, and `o` goes to
its other end. On the selection, `d` (or `x`, `Delete`), `c` (or `s`), `y`,
`>`, `<`, `J`, `~`, `u` (lower case), `U` (upper case), `r` and a character,
and `p` or `P` (put in its place; `p` keeps what it replaced) act on it, and
`D` `X` `C` `S` `R` `Y` on its whole lines. A count before `>` or `<`
indents that many times.

### Registers

`"` and a name before a command chooses where it yanks, deletes or puts:
`"a` to `"z`, with `"A` to `"Z` adding to them; `"0`, the last yank; `"_`,
which keeps nothing; and `"+` or `"*`, the clipboard. Without a name, yanks
and deletes go to the unnamed register, which `p` puts; they stay inside
parch, and every window shares them. `Ctrl+C` and `Ctrl+V` still copy and
paste through the clipboard, and in Visual mode they act on the selection.

### Search

`/` and `?` search forward and backward for what you type, and `Enter` goes
there; `n` and `N` go on in the same or the other direction, and `*` and `#`
look for the whole word under the cursor. A pattern is plain text, not a
regular expression, and its letters match either case unless it has a
capital; `*` and `#` match the word's case. A search goes round the end of
the document, and the status line says so; one that finds nothing says that
instead. A search is also a motion: `d/the` and `Enter` deletes up to the
next "the".

### The command line

`:` opens it in the status line; `Backspace` takes back a character, `Ctrl+V`
pastes, `Enter` runs it and `Esc` leaves it.

| Command | What it does |
| --- | --- |
| `:w`, `:write` | Save, as `Ctrl+S` does |
| `:q`, `:quit` | Close the tab, which saves it first or asks, as closing a tab always does |
| `:wq`, `:x`, `:xit`, `:exit` | Save and close the tab; a draft is named first, and its tab stays open until you `:q` |
| `:q!`, `:wq!`, `:x!` | The same as without `!`: parch never throws text away |
| `:` and a number | Go to that line; `:$` goes to the last |

Closing a window's last tab closes the window, and closing the last window
quits parch, which first saves everything or keeps it in recovery.

### Undo

Every Normal or Visual mode command is one step for `u` to take back, and so
is a stay in Insert mode together with the command that began it: `cw`, what
you typed and the `Esc` undo at once. A change that `.` repeats is one step
too; `.` repeats a Visual mode change on as much text from the cursor as it
changed.

Not covered: other Vim commands, such as `gj`, `ge`, `R`, block selection,
marks, macros and folds; regular expressions, `:s` and line ranges; Vim's
settings and mappings.

## Keep your writing safe

parch saves as you write, keeps text that is not yet in its file through a
crash, keeps earlier versions of every document, and never saves over a file
that changed on disk without asking.

### Saving

A named document saves itself a moment after you pause typing, and at least
every five seconds while you keep going. `Ctrl+S` saves at once.

- Saves replace the file in one step: the new text is written beside it and
  then put in its place, so the file is never left half-written. Its
  permissions, group, extended attributes and access lists are kept.
- A file with several hard links, or one whose owner or group a new file
  could not keep, is written in place instead, after its current text is in
  history.
- Saving through a symbolic link you opened keeps saving through it.
- When a save fails, the notice bar says why and offers **Retry** and
  **Save as…**. Your text stays in the window and in recovery.

### Recovery

Text that is not in its file yet is copied to recovery within half a second
of a pause. If parch or the computer stops, the next launch brings the text
back and says so. Recovery for a document is cleared once its file holds the
text. An unresolved conflict keeps the text shown in the editor in recovery
while you decide what to do with the file on disk.

### History

parch keeps earlier versions of each document. Open them with
**Version history…** from the menu (`F10`).

- A version is kept every five minutes while you write, when a document is
  first saved after opening, and before anything replaces its text: a reload,
  an overwrite, a restore, or text brought back from recovery. Each version
  says why it was kept.
- **Restore** replaces the text with the chosen version; `Ctrl+Z` takes it
  back.
- A file you reopened in another encoding and saved as UTF-8 keeps its
  original bytes as a version, shown as that encoding reads them, and saying
  so. **Restore** on it puts those bytes back into the file as they were; the
  file then opens read-only again, and **Reopen as…** reads it once more. The
  text it replaced is kept as a version too.
- **Name this version** keeps a version until you delete it. Unnamed versions
  are kept for 30 days, at most 100 per document, within 1 GB in all. Under
  **History** in **Preferences…** you can change all three. A higher limit
  applies at once. A lower one waits until you stop changing it; if it would
  remove older versions, Preferences say how many, and they go only when you
  choose **Remove them**. **Keep them**, or closing Preferences, leaves the
  limits as they were. A version you named is never removed.
- If named versions alone fill that space, history pauses and the notice bar
  says so. Saving carries on.

### Changes on disk

parch watches the files you have open.

- If a file changes on disk and you have not edited it in parch, parch
  takes the new text. `Ctrl+Z` still steps back through it.
- If you have unsaved changes too, the document goes into **conflict** and
  nothing is saved over the file until you choose. You can keep writing
  meanwhile. **Review** (or `Ctrl+S`) shows your text beside the file's,
  with the differing lines marked, and offers:
  - **Reload**: take the file's text. Yours stays in history and undo.
  - **Keep both**: save your text as a copy beside the file, and take the
    file's text.
  - **Overwrite**: save your text over the file. The file's text goes to
    history.
  - **Save as…**: put your text somewhere else.
- If another program renames the file, or moves it into a folder parch
  watches (that of another open file, or a library folder you have open),
  the tab follows it, with any unsaved text, its history and its place among
  the recent files, and a notice says so. Moved anywhere else, the file is
  missing, as if it had been deleted.
- If the file was deleted, **Save again** puts your text back where it was.
- If the file was replaced by a link to another file, nothing is read from
  or written to that file unless you choose **Follow the link**. **Put this
  text at “name”** replaces the link with a file holding your text.

## Choose how a document is read

**Document settings…**, in the menu (`F10`) or the command palette, shows how
the document in front is read, and where each setting comes from:

- **Markdown**: **Writer-compatible**, the default, with the syntax of iA
  Writer and parch's extensions; or **Standard**, CommonMark 0.31.2 with
  GFM tables, strikethrough, task lists and bare web addresses as links.
- **Extensions**: what the Markdown is read with beyond the profile's own,
  such as footnotes, citations, math, Mermaid diagrams, `//` comment lines,
  front matter and included files, each on or off.
- **Citation style** and **Bibliography**, described in
  [Citations and bibliographies](#citations-and-bibliographies).
- **Library root**: the folder that includes starting with `/` are found in;
  the library folder that holds the document unless you choose another.
- **Includes**: the limits that keep included files from running away: how
  deep includes may nest (**Depth**, 32 by default), how much text the
  document and everything it includes may hold together (**Size**, 100 MiB),
  and how many includes are read in all (**Files**, 10,000). An include past
  a limit is not read, the preview and **Check document** say which and why,
  and an export of the document is refused.

Each setting is the first of these that has one: the document's own
settings; its front matter (the bibliography and the citation style only);
the settings of the library folder that holds it; **Markdown and citations**
in **Preferences…**, for every document; and the defaults. Under each
setting the dialog says which it is.

Choose **This document** to change the document alone, or **Every document
in the library folder** to change every document in it; a setting of the
document itself still wins over its folder's. **Use the default** gives a
setting back to the next of these. A change applies at once to the preview,
live formatting, spelling, the outline, the word count and **Check document**. It
changes how the text is read, never the text: parch keeps these settings
itself, in `~/.local/share/parch/document-settings.json`, and they follow a
document when parch renames or moves it. A draft has settings of its own
from the start, and a library folder once it is saved in one.

## Keyboard shortcuts

The command palette (`Ctrl+Shift+P`) lists everything parch can do. Type a
few letters of a command's name, in order, to find it; the arrows choose and
`Enter` runs it.

**Keyboard shortcuts…** in the menu lists every command and its keys. Choose
one and press **Change…** (or `Enter`), then the keys you want. Keys that
type text, and keys kept for editing such as `Ctrl+C`, are refused; keys
another command has can be swapped. **Remove** leaves a command without keys,
**Reset** gives it its own back, and **Reset all** every command.

| Key | Does |
| --- | --- |
| `Ctrl+N` | New draft |
| `Ctrl+Shift+N` | New window |
| `Ctrl+O` | Open a file |
| `Ctrl+Shift+O` | Find a document |
| `Ctrl+F` / `Ctrl+H` | Find / find and replace |
| `F3` / `Shift+F3` | Next / previous match |
| `Ctrl+Shift+F` | Search the library |
| `Ctrl+R` | Go to heading. Redo or cancel a pending command in the focused, editable Vim editor in Normal or Visual mode |
| `Ctrl+S` | Save now |
| `Ctrl+Shift+S` | Save as |
| `Ctrl+Shift+E` | Export |
| `Ctrl+P` | Print |
| `Ctrl+W` | Close the tab |
| `Ctrl+Q` | Quit |
| `Ctrl+V` | Paste, rich text as Markdown |
| `Ctrl+Shift+V` | Paste plain text |
| `Ctrl+Z` | Undo |
| `Ctrl+Shift+Z` / `Ctrl+Y` | Redo |
| `Ctrl+Tab` / `Ctrl+PageDown` | Next tab |
| `Ctrl+Shift+Tab` / `Ctrl+PageUp` | Previous tab |
| `F9` | Show or hide the library |
| `Ctrl+Alt+O` | Show or hide the outline |
| `Ctrl+Alt+1` / `Ctrl+Alt+2` / `Ctrl+Alt+3` | Source / split / reading view |
| `Ctrl+Alt+P` | Show or hide the preview |
| `Ctrl+Alt+E` | Switch between the text and the preview |
| `Alt+Left` | Back in the preview |
| `Ctrl+=` / `Ctrl+-` | Larger / smaller text |
| `Ctrl+0` | Theme text size |
| `F6` / `Shift+F6` | Move between the library, tabs, text, preview and notices |
| `Ctrl+Shift+D` | Focus mode |
| `F11` | Distraction-free writing |
| `Menu` / `Shift+F10` | Spelling suggestions for the word at the caret |
| `F10` | Open the menu |
| `Esc` | Close the find bar, leave distraction-free writing, or dismiss a notice |
| `Ctrl+Shift+P` | Command palette |
| `Ctrl+,` | Preferences |
| `F1` | User guide |
| `Ctrl+B` / `Ctrl+I` | Bold / italic |
| `Ctrl+Shift+X` / `Ctrl+Shift+H` | Strikethrough / highlight |
| `Ctrl+E` / `Ctrl+/` | Inline code / comment |
| `Ctrl+1` … `Ctrl+6` | Heading 1 to 6 |
| `Ctrl+'` | Quote |
| `Ctrl+Shift+L` / `Ctrl+Alt+L` | Bulleted / numbered list |
| `Ctrl+L` | Check or uncheck task |
| `Ctrl+Shift+K` | Code block |
| `Ctrl+K` / `Ctrl+Shift+I` | Link / image |
| `Ctrl+Alt+F` | Footnote |
| `Ctrl+T` | Table |
| `Ctrl+M` / `Ctrl+Shift+M` | Inline / display math |
| `Ctrl+Enter` | Page break |
| `Shift+Enter` | Hard line break |

The keys of every command can be changed with **Keyboard shortcuts…**.

## Command line

```text
Usage: parch [open] [--theme DIR] [--benchmark OUT] [FILE...]
       parch COMMAND [options]
       parch --screenshot PNG [OPTION...] [FILE...]

  open             Open the files that follow (optional; bare files also open)
  --theme DIR      Show the Omarchy theme in DIR, or the installed one so named
  --default-theme  Show Omarchy's default theme as it ships, not the desktop's
  --benchmark OUT  Measure the window on a copy of one FILE; write JSON to OUT
  --screenshot     Draw the window off screen, save it as PNG once all of it
                   is drawn, and exit; nothing parch keeps is changed
  --version        Print the version and exit
  --help           Print this help and exit

With --screenshot:
  --size WxH       The window's size, such as 1280x800
  --view VIEW      The text (source), both (split) or the preview (reading)
  --show PANEL     Open export, print, document-settings, history, palette,
                   outline, preferences or shortcuts
  --library        Show the library
  --no-library     Hide the library
  --focus          Turn focus mode on
  --line N         Put line N at the top of the text and of the preview
  --scroll AMOUNT  Scroll that far down instead, from 0 (the top) to 1
  --select A,B     Select the text from offset A to offset B

With --benchmark:
  --view VIEW      As with --screenshot; source unless given
  --windows N      1, or 2 windows showing the same FILE
  --adversarial    Also a 1 MB paste and its undo, deep nesting and rapid keys

Commands that need no display (parch help COMMAND for its options):
  check FILE...    Report what is wrong in documents, with file, line and column
  render FILE      Write a document's HTML, plain text, outline or semantic tree
  export FILE      Write a document as Markdown, HTML, PDF, DOCX or EPUB
  stats FILE       Count words and characters, reading time and headings
  skill [install]  Print the skill for coding agents, or install it
  help [COMMAND]   Print this help, or a command's options
```

Files and folders given to `parch` while it is running open in the running
window. A folder is added to the library. A file named like a command opens
with `parch open check` or `parch -- check`.

### Checking, rendering and exporting

The commands read a document together with the files it includes. They need
no display, never change the document, and print text for people or, with
`--json`, JSON for scripts. `parch help COMMAND` lists a command's options.

```sh
parch check notes.md chapters/*.md   # What is wrong, file by file
parch check notes.md --no-render     # The same, without drawing formulas and diagrams
parch render notes.md --to outline   # html, text, outline or json-tree
parch export book.md -o book.html    # A page of its own
parch export book.md -o book.pdf --page-size Letter
parch export book.md -o book.docx    # Also .epub for a book
parch export book.md -o whole.md     # One Markdown file with its includes in place
parch stats book.md                  # Words, characters, reading time, headings
```

- **check** reports each problem as `file:line:column: severity: message
  [code]`: includes that cannot be read, references, footnotes, citations and
  variables that lead nowhere, images whose files are missing, links to files
  or headings that are not there, malformed table rows, unclosed math, front
  matter it cannot read, HTML that is shown as text, and what is wrong with
  citations, bibliography files and citation styles (a problem in a
  bibliography file is reported at its line there). Notes of severity
  `info` (HTML shown as text, a footnote nothing refers to) are listed but do
  not count as problems. Every formula and Mermaid diagram is drawn as the
  preview draws it, and one that RaTeX or Merman refuses is reported
  (`invalid-math`, `invalid-diagram`) with what the helper said; when a
  helper cannot be found, check says what it could not check and does not
  count it as a problem. With `--json` it reports the RaTeX and Merman
  versions under `renderers`.
- **render** writes the HTML fragment the preview and exports are made from
  (`html`, the default), the text a reader sees (`text`), the headings
  indented by level (`outline`), or the whole semantic tree with source ranges
  (`json-tree`). Relative links and images lead from the folder the HTML is
  written to, or from the document's.
- **export** writes the document in any of the formats the window's
  [Export](#export-and-print) writes, in the same way: `md`, the document
  as one file with each include in place (an included file's front matter
  left out and its variables written out, its footnote and link labels kept
  apart from the document's, and its paths written from the new file's
  folder. Markdown’s `--images auto` copies pictures that would otherwise
  be outside the exported document’s folder. Use `--images copy` to copy
  all pictures, `--images link` to keep their original locations, or
  `--as-is` to write the original text); `html`, a standalone page; `pdf`, set on pages;
  `docx`, a Word document; or `epub`, a book. Each format's options are in
  `parch help export`, such as `--page-size`, `--margins`, `--font` and
  `--font-size` for PDF, `--notes endnotes` for DOCX and `--images
  copy|embed|link` for HTML. Formulas and diagrams are drawn with parch's
  helpers when they are installed. What a format leaves out of the document
  is printed as a note. The format follows `-o`'s name unless `--to` gives
  one.
- **stats** counts the words, characters (with and without spaces) and
  headings a reader reads, and the reading time at 200 words a minute.

An export never writes over the document or a file it includes, never
writes through a symbolic link, even with `--force`, replaces a file that
is there only with `--force`, and is not made at all when a file the
document includes cannot be read. `--quiet` prints nothing but errors.
`render -o` follows the same file rules. `render` to standard output can
preview an incomplete document and reports that an include could not be read.

### The dialect

Every command reads the Writer profile with all of its extensions, whatever
the window was last set to, so a command gives the same result on any
machine. These options change it, the highest first:

- `--profile writer|standard`, `--extension NAME` and `--no-extension NAME`
  (given again for more); `parch help check` lists the extensions.
- `--library DIR` sets the root for `/rooted` includes and gives the
  document access to pictures in that folder and its subfolders.
- `--bibliography FILE` (given again for more) and `--csl STYLE`: the
  bibliography and the citation style, a bundled one or a `.csl` file.
- `--max-include-depth N`, `--max-include-size SIZE` (bytes, or with `K`,
  `M` or `G`, as in `20M`) and `--max-includes N`: the include limits of
  [document settings](#choose-how-a-document-is-read).
- `--use-saved-settings`: read the document as the window reads it, with the
  [document settings](#choose-how-a-document-is-read) kept for the document, its library
  folder and every document, and the library folder for `/rooted` includes.
  What the options say still wins.

The front matter's `bibliography:` and `csl:` choose the bibliography and the
style unless the options, or with `--use-saved-settings` the document's own
settings, choose them. JSON results report the profile, the extensions, the
bibliography, the style and the include limits, where each came from, the
dialect version, and the version of what drew the result.

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Done, and nothing wrong |
| 1 | The command line is wrong |
| 2 | `check` found problems, or `export` or `render -o` could not read an include |
| 3 | A file could not be read or written, an argument was invalid UTF-8, or an output needs `--force` |
| 4 | The export could not be made |

### Pictures of the window

`--screenshot PNG` draws the window as the options after it set it up, off
screen, and saves it once everything in it is drawn: the preview with its
formulas and diagrams, spelling and live formatting, and any panel it opens.
No window shows. It reads copies of your settings and changes nothing parch
keeps, and it neither hands files to a running parch nor takes them from one.

```sh
parch --screenshot notes.png --size 1280x800 --view split --line 40 notes.md
parch --screenshot export.png --default-theme --show export notes.md
```

### For coding agents

`parch skill` prints a skill that tells coding agents how to write, check,
render and export your documents with these commands. `parch skill install`
installs it as `~/.agents/skills/parch/SKILL.md`, with a link to it from
`~/.claude/skills/parch` when `~/.claude` exists. Running it again updates
it; other skills, and a file or link there that is not parch's, are left
alone.

## Settings

**Preferences…** (`Ctrl+,`) sets the writing font, any installed family or
the theme's; the text size; the line height; the longest line, in
characters; whether the Markdown is styled as you write; how headings are
written; whether the formatting toolbar shows; whether parch starts with a
new draft in front of the documents it reopens (**Start with a new
draft**); whether spelling is checked;
what focus mode keeps bright, and how strongly it dims the rest; and how many
versions history keeps, for how long and in how much space. A window too
narrow for the toolbar hides it until you ask for it with **Show toolbar**.
Every change applies at once in every window. Under **Keys**, **Edit with Vim
keys** turns on Vim keys. Under **Markdown and citations**, the Markdown
profile, extensions, citation style and bibliography for every document, as
[Document settings](#choose-how-a-document-is-read) describes them.

Under **Preview** it sets the reading font, which is a serif made for reading
unless you choose another; the preview's text size, two pixels larger than
the text's until you choose one; its line height and longest line; whether
the text and the preview scroll together; and whether pictures from the
web are shown in every document.

## Appearance

parch draws everything from your Omarchy theme and follows it as you switch
themes, light or dark, along with the theme's font. `Ctrl+=` and `Ctrl+-`
change the text size, and `Ctrl+0` returns to the theme's size.

Screen readers and other assistive technology reach every control by name.
They can also edit the text, place the caret and select: each edit they
make is one step for `Ctrl+Z`, as typing is.

## Your data

Your documents are ordinary Markdown files, saved where you put them, with
their line endings kept. Everything else parch keeps is in these folders:

| Path | Contents |
| --- | --- |
| `~/.local/share/parch/drafts/` | Drafts, one folder each |
| `~/.local/share/parch/history/` | Version history, one folder per document |
| `~/.local/share/parch/documents.json` | Which history belongs to which file |
| `~/.local/share/parch/dictionary.txt` | Words added to the spelling dictionary |
| `~/.local/share/parch/document-settings.json` | Document settings, by document, library folder and for every document |
| `~/.local/share/parch/exports.json` | Each document's last export, which Export again repeats |
| `~/.local/state/parch/recovery/` | Text not yet in its file |
| `~/.local/state/parch/session.json` | Open windows and tabs |
| `~/.local/state/parch/recent.json` | Recent files |
| `~/.local/state/parch/positions.json` | Where you were in each document |
| `~/.config/parch/settings.json` | Settings, such as library folders, text size and your own keys |
| `~/.cache/parch/render/` | Formulas and diagrams already drawn, at most 128 MiB |

These follow `XDG_DATA_HOME`, `XDG_STATE_HOME`, `XDG_CONFIG_HOME` and
`XDG_CACHE_HOME` when they are set.

### Back up and restore

Back up your documents as you always do, and with them parch's folders:
`~/.local/share/parch` holds your drafts, version history, document settings
and the words you added to the dictionary, and `~/.config/parch` your
settings and keys. `~/.local/state/parch` holds what parch was doing (open
tabs, recent files, text not yet saved), and `~/.cache/parch` only drawings
it can make again.

To restore, or to move to another computer, quit parch, put the folders back
in the same places, and start it. History follows a document by its path, so
put your documents back at the same paths too; a document at another path
starts a history of its own.

### Privacy

There is no account, and nothing you write leaves your computer. The one
thing parch fetches from the network is a picture a document names by web
address, for the preview, and only once you ask: fetching a picture tells
its server that the document was opened, when, and from where. A document
that names such pictures says **This document shows pictures from the web**
above its preview, and **Show them** fetches them for that document, now
and whenever it opens again; the choice is kept with its
[document settings](#choose-how-a-document-is-read). **Fetch pictures from
the web** under **Preview** in **Preferences…** shows them in every
document. Exports never fetch pictures.

Even when pictures are shown, parch fetches only from public addresses:
never from your computer itself, your local network, or link-local and
shared addresses, whatever name a document gives them, and it checks every
address a name leads to, again after each redirect. It follows at most
three redirects, never from `https` to `http`, takes only PNG, JPEG, GIF,
WebP and SVG pictures, at most 32 MiB of one, and gives up on a picture
that takes more than 30 seconds. The preview connects directly to a checked
address. On a proxy-only network, save the picture beside the document
and use its local path.

parch's own folders, which hold copies of what you write (drafts, history
and recovery) and what you were doing (open tabs, recent files, settings),
are readable by your account alone, whatever your umask, and parch makes
ones an earlier version left open private again as it starts.

A web or mail link you click in the preview opens in your browser or mail
program; a link of any other kind asks first.

Formulas and diagrams are drawn by helper programs that run on your
computer, cut off from the network, each in a private temporary folder,
with at most 2 GiB of memory and 30 seconds of processor time, and stopped
after 15 seconds. Cutting a helper off from the network needs unprivileged
user namespaces, which some hardened kernels turn off; there the helpers
run with the network, and `parch check --json` says so with
`"isolated": false` under `helpers`.

## Troubleshooting

**Installing says parch-writer and parch are in conflict.** The unrelated
AUR package `parch` is installed; both have a `parch` command. See
[Another package called parch](#another-package-called-parch).

**A formula or diagram shows its source.** It is being drawn, or it cannot
be: the reason is shown in its place, and **Check document** lists every one.
A TeX command parch cannot draw is listed under [Math](#math), and a Mermaid
syntax error is said as Merman reports it. If none is drawn at all, the
helpers are missing from `/usr/lib/parch`: install the package again.

**Words are not underlined, or parch says spelling is not checked.** The
English dictionary is missing: `sudo pacman -S hunspell-en_us`. Spelling may
also be turned off: **Check spelling** in the menu turns it on.

**Open, Save as or Print says the dialog cannot be reached.** parch asks the
desktop for these through `xdg-desktop-portal`. Make sure it and a portal
backend for your desktop are installed and running; Omarchy has both.

**A file opens read-only.** It is not valid UTF-8: **Reopen as…** reads it
in the encoding it was written in (see
[Start and open documents](#start-and-open-documents)). A file larger than
64 MiB is not opened at all, and parch says so.

**parch says it restored text from recovery.** parch or the computer
stopped before the text reached its file. The text is back in its tab; save
it, or look in **Version history…** for what the file held.

**The document is in conflict.** The file changed on disk while you had
unsaved changes. **Review** shows both and lets you choose (see
[Changes on disk](#changes-on-disk)).

**The notice bar says history is paused.** Versions you named fill the space
history may use. **Delete version** in **Version history…** removes one, or
give history more
room under **History** in **Preferences…**. Saving carries on meanwhile.

**A picture does not show in the preview.** A picture from the web needs a
network connection and **Show them** above the preview, or **Fetch pictures
from the web** turned on in **Preferences…**; one on your own computer or
local network is never fetched, nor one that is not a PNG, JPEG, GIF, WebP
or SVG picture. Local pictures must be inside the document's folder or its
library, including after following symbolic links. For a shared `../images`
folder, add the common parent as a library or pass `--library DIR` to the
terminal command. **Check document** lists missing and out-of-scope pictures.

**An included file is not shown.** It cannot be found, it includes itself, it
is outside the library or the document's folder (see [Includes](#includes)),
or it passes a limit in [Choose how a document is read](#choose-how-a-document-is-read).
The preview marks it, and pointing at it says why.

**Citations show as their keys.** No bibliography defines them, or the
document has no citation style. `parch check` names each key and what is
missing (see [Citations and bibliographies](#citations-and-bibliographies)).

**Opening a file from the terminal adds a tab instead of a window.** A
running parch takes the files it is given. `Ctrl+Shift+N` opens a new window,
and **New view** shows one document in two tabs.

**`parch export` refuses to write a file.** The file is already there:
add `--force` to replace it. It never writes over the document itself, and
never through a link: when the file is a link, export to another name, or
remove the link first.

## Conformance

### Profiles

parch reads Markdown in one of two profiles, chosen in
[document settings](#choose-how-a-document-is-read) or with `--profile`:

- **Writer-compatible**, the default: the syntax of iA Writer (merged table
  cells, inline notes, `//` comment lines, `+++` page breaks, included
  files, heading labels, ordered lists numbered from 1) and parch's
  extensions (math, Mermaid, definition lists, footnotes, citations,
  highlights, superscript and subscript, `{{TOC}}`, front matter and
  variables), on top of CommonMark and GFM.
- **Standard**: CommonMark 0.31.2 with the four GFM extensions, tables,
  strikethrough, task lists and bare web and e-mail addresses as links,
  and nothing else.

### CommonMark and GFM

parch's own Markdown reader is checked against the examples of the
specifications, each rendered to HTML and compared with the HTML the
specification gives:

- CommonMark: with bare addresses read as text, as CommonMark has them,
  parch gives the specification's HTML for all 652 examples of the
  CommonMark 0.31.2 specification. In the Standard profile as it is, with
  GFM's bare links, 648 of the 652 pass; examples 602, 608, 611 and 612 differ
  by design, as GFM makes a link of a bare `https://…` or e-mail address that
  CommonMark leaves as text.
- GFM: all 23 examples of the GFM 0.29 extensions pass (8 tables, 2 task
  list items, 2 strikethrough and 11 autolinks), in both profiles; parch
  writes the task box's `<input … />` closed, as it writes `<br />`.
- The Writer-compatible profile gives the same HTML for 637 of the 652
  CommonMark examples. The rest differ where Writer syntax means something
  else: `+++` is a page break, `---` at the very top starts front matter, an
  ordered list is numbered from 1 whatever its first number, and bare
  addresses are links.

Those comparisons pass raw HTML through, as the specification's own HTML
does. parch itself never does: under its HTML policy (below), 579 of the 652
examples give the same HTML. The other 73 are the four bare-address examples
above and 69 that hold raw HTML parch shows as text, attributes it leaves
out, or HTML comments, which it leaves out of what it shows.

### Raw HTML

HTML in a document is read under one policy, in the preview, the HTML
export and `parch render` alike. These elements are supported and shown as
what they mean:

- **Text level**: `a`, `b`, `i`, `em`, `strong`, `u`, `s`, `del`, `ins`, `mark`, `code`, `kbd`, `sub`, `sup`, `br`, `img`
- **Blocks**: `p`, `blockquote`, `pre`, `h1`, `h2`, `h3`, `h4`, `h5`, `h6`, `ul`, `ol`, `li`, `dl`, `dt`, `dd`, `hr`, `table`, `caption`, `thead`, `tbody`, `tfoot`, `tr`, `th`, `td`

They keep only these attributes; any other is left out, and a link or image
whose address is a script loses it:

| Element | Attributes kept |
| --- | --- |
| `a` | `href`, `title` |
| `img` | `src`, `alt`, `title`, `width`, `height` |
| `ol` | `start` |
| `td`, `th` | `colspan`, `rowspan`, `align` |

Every other element, such as `<div>`, `<span>`, `<script>`, `<style>` or
`<iframe>`, is shown as the text it is, and nothing in a document ever runs.
A tag without its partner is shown as text too. HTML comments are kept in
the text and left out of what is shown. **Check document** and `parch check`
report each of these as a note.

## What parch does not do

- **Hide the Markdown.** Every `#`, `*` and `[link](…)` stays in the text;
  the preview is where it reads as formatted.
- **Sync or share.** There is no account, cloud or collaboration; put your
  documents in a folder another tool syncs if you want them elsewhere.
- **Languages other than English.** The window, spelling and the bundled
  citation locales are English.
- **Every Vim command.** See what [Vim keys](#edit-with-vim-keys) cover;
  macros, marks, folds, block selection and `:s` are not among them.
- **Run anything in a document.** Raw HTML parch does not support is shown as
  text, and nothing in a document ever runs.
- **Word equations.** Formulas and diagrams are pictures in a DOCX file.
- **Fetch web pictures for exports.** A PDF or EPUB shows the picture's
  description instead; an HTML page links to it.
- **Come packaged for other systems.** The packages are for Arch Linux and
  Omarchy.
- **Keep up as quickly in one enormous paragraph.** A key typed in a single
  paragraph of 10,000 words takes about 60 ms to show, against 4 ms when the
  same words are split into paragraphs; break such text up as you write.
- **Lay out text no one could read.** A word longer than 10,000 characters,
  with no space or hyphen in it, makes the editor wrap that document at any
  character rather than between words, since breaking such a word between
  words takes Qt time that grows with the square of its length; the
  document wraps between words again once it is opened afresh without it.
  The preview and PDF give such a word a place to break every 512
  characters, which copying leaves out. A table of more than 128 columns or
  20,000 cells is shown in the preview and printed as it is written, in a
  code block, rather than laid out as a table. A PDF that takes long to set
  shows how far it has come and stops when cancelled.

## Getting help

Found a bug, or stuck on something this guide does not cover? Open an
[issue](https://github.com/willykev/parch/issues).
