# Changelog

## Unreleased

- Recovery keeps the text shown in an unresolved conflict, even if an
  externally deleted file quickly returns with different contents.
- Vim commands survive modifier presses. Character commands compose dead
  accents without enabling an input method and cancel if there is no single
  composed character. `Ctrl+R` redoes edits or cancels a pending command in
  the focused, editable Vim editor. It opens Go to heading in read-only
  documents.
- Library moves and renames keep closed documents' version history.
  Link updates preserve encoded names, quoted includes and local file URLs.
  Quoted includes accept Markdown punctuation in filenames. Quoted
  addresses stay as text or links. The Move to picker opens on an enabled
  folder, and heading labels show literal percent placeholders.
- File renders refuse incomplete documents and preserve an existing output.
  Assembled Markdown preserves heading targets and list boundaries. DOCX
  links use internal bookmarks, and HTML and EPUB give
  footnotes their own IDs. The preview keeps its note and bibliography
  targets distinct from heading slugs.
- Links inherited from a root document resolve from their definition.
  HTML heading links follow the same anchor rules as Markdown links.
  Plain text omits unused notes, and stats counts visible code as text.
- Draft filenames use the visible text and portable filename characters.
  The CLI reports invalid UTF-8 arguments and distinguishes unreadable
  pictures from pictures outside the allowed folders.

- Re-exporting HTML or Markdown preserves files in the pictures folder, including
  included chapters and pictures edited since the last export. Concurrent CLI
  exports require `--force` to replace an output.
- Local pictures follow the same folder limits as includes. HTML and EPUB
  carry supported `<img>` elements, and web pictures connect to a checked
  address on every redirect.
- Headless export works with a GTK desktop theme. Screenshot and benchmark
  outputs refuse input documents and their aliases. The CLI and includes
  report incomplete UTF-8 at the end of a file.
- Cross-filesystem moves check that the whole file was copied before removing
  the original. Updating links in a closed document keeps its previous bytes
  in history before writing.
- No render or export writes over a file the document is made from: its
  bibliographies and citation style, and the pictures it shows, as well as
  the files it includes.
- PDFs, EPUBs, HTML and DOCX lead links to an included chapter to that
  chapter inside them, at the heading the link names, and a PDF never turns
  a link into a `file://` address with the writer's folders in it. A `#link`
  in an included chapter reaches that chapter's own heading, and EPUB
  chapters route links to headings with accented names.
- Assembled Markdown rewrites bibliography and style paths for its output
  folder and adds bibliographies from included chapters. It omits and
  reports citation files named in front matter outside the source's
  allowed folders. It adds effective settings only when the text uses
  citations.
- Replacing an export keeps the old file's group and extended attributes,
  its access control list among them, as well as its mode.
- Web pictures are never fetched from this computer's own addresses or from
  a network it is directly on, public or not.
- HTML pictures load when the output's name has a `#`, `?` or `%` in it, each
  picture keeps its SVG view (`icons.svg#home`), and diagrams no longer fail
  when the default font is a WOFF or WOFF2 file.

## 0.1.1 — 2026-11-17

### Documentation

- The README shows a Mermaid flowchart drawn again as each line of it is
  typed, and a pie chart beside its source.
- The example write-up installed in `/usr/share/doc/parch/examples` has a
  larger flowchart and a pie chart of what the weather station cost.

## 0.1.0 — 2026-11-16

The first version: a native Markdown writing app for Omarchy.

### Writing

- Markdown as plain text, with its punctuation in view, in a window that
  follows the Omarchy theme and font, light or dark.
- Live formatting: headings, bold, italic, strikethrough, highlights, links,
  images, code, math, notes and citations styled as you write, in the
  theme's colours, with every mark of Markdown kept in the text and dimmed,
  and the caret and line breaks never moved. It can be turned off.
- Tabs, separate windows, and several views of one document.
- Drafts that are kept from the first keystroke and named when saved.
- Each file's line endings and byte-order mark are kept.
- A file that is not UTF-8 opens read-only, and Reopen as… reads it in
  Windows-1252, Latin-1, UTF-16 or any other encoding the system knows, with
  a preview; it is written as UTF-8 only once you confirm, its original
  bytes kept in history first, where Version history shows them in that
  encoding and can put them back.
- Rich text pastes as clean Markdown; `Ctrl+Shift+V` pastes plain text.
- Pasted images and dropped files go into an `assets` folder beside the
  document; a dropped Markdown document can be linked or included.
- A pasted image with no alt text leaves the caret between `![` and `]`, to
  describe it there.
- Assistive technology can edit the text, move the caret and select, each
  edit one undo step.
- A note reference that nothing defines can be turned into an inline note,
  or given a definition, from the editor's menu on it or the command
  palette.
- A draft saved beside another document's assets gives its own assets a free
  name (`-2`, `-3`) where another file has theirs, and updates its links to
  them, as one undo step, before the file is written.
- Check document lists everything wrong in a document and the files it
  includes, as file, line, column and message, without holding up the
  window: links, images and includes whose files are missing, links to
  headings that are not there, references, notes, citations and variables
  that lead nowhere, include cycles and limits, formulas and diagrams that
  cannot be drawn, and more. `Enter` opens the place, in the included file
  when the problem is in one.
- Formatting commands that write and toggle Markdown, each one undo step, from
  the menu, a formatting toolbar, a command palette and their keys, among
  them superscript (`^…^`) and subscript (`~…~`).
- Lists that continue on Enter and indent with Tab, table cells reached with
  Tab, table formatting, and Shift+Enter for a hard line break. A list of
  repeated `1.` keeps its style, a new item in a list that counts up
  renumbers the items after it, and Renumber list counts a list up again.
- Keys you can change, with refusals for keys that type and swapping for keys
  taken.
- English spell checking as you write, with wavy underlines, suggestions, a
  personal dictionary and Ignore, from a right click or the `Menu` key.
- Focus mode, which dims all but the sentence or paragraph being written, and
  distraction-free writing, which shows the text alone. Dimming blends the
  text into the background, light or strong or off, and never further than
  3:1 against it in any theme.
- A Markdown guide, from Help in the menu or the command palette: every
  part of the dialect as an example you can edit, beside its preview, with
  the cases that surprise. Nothing written in it is kept; Save as keeps a
  copy.
- Preferences for the writing font, text size, line height, line length,
  live formatting, heading syntax, toolbar, spelling and focus mode, applied
  at once in every window, and for starting each launch with a new draft.
- A word count and reading time for the document or the selection, of the
  text a reader sees.
- Document settings for each document, kept by parch and never in the file:
  the Markdown profile (Writer-compatible or Standard), its extensions, the
  citation style, the bibliography, the library root and the limits on
  includes (depth, size and number), with defaults for a
  library folder and, in Preferences, for every document. The dialog shows
  where each setting comes from, and live formatting, spelling, the outline,
  the word count and Check document read the document as they say.
- Live formatting, spelling, the preview, the word count, the outline and
  Check document share one reading of each revision of a document, made off
  the typing path with its included chapters and bibliography, so that they
  agree with one another and a key costs one parse of the text, not one for
  each.
- Citations and bibliographies: Writer `[p. 23][#key]` and Pandoc
  `[@key, p. 23]` citations, with words before each key
  (`[see @doe99, p. 33; also @roe04]`), `[-@key]` without the author and
  `@key` in the sentence for the author's name there; Writer
  bibliography entries written in the document, and BibTeX, BibLaTeX, CSL
  JSON and CSL YAML files, chosen in Document settings or with
  `bibliography:` and `csl:` in the front matter. Seven bundled CSL styles
  (APA, Chicago author-date and notes, IEEE, MLA, Harvard, Vancouver) or a
  `.csl` file of your own; note styles put citations in footnotes. Only
  English locales are bundled, CSL-M styles are refused, and 746 of the 809
  English fixtures of the CSL test suite pass; the user guide says so.
- Optional Vim keys, off until chosen: Normal, Insert and Visual modes;
  motions, operators and text objects with counts; registers, `.` and undo by
  whole changes; search; and `:w`, `:q`, `:wq` and `:x`. The status line
  shows the mode, and `Esc`, the command palette and the menu always lead
  back out.

### Reading

- A preview of what the document reads as, in a reading font and the theme's
  colours: beside the text in a split with a divider you can drag or move
  with the keys, or alone in a reading view. Each tab keeps its view.
- Everything the Markdown dialect has is shown: tables with alignment and
  merged cells, code coloured by language, task lists, definition lists,
  footnotes with links both ways, citations and a bibliography, a table of
  contents, images, math and Mermaid diagrams, and included files.
- Math in the LaTeX KaTeX 0.16 supports, tested with an expression for each
  command on its list: 1,037 of 1,043 draw. The user guide names the rest
  and the other differences.
- The user guide's conformance section gives both profiles' results on the
  CommonMark 0.31.2 and GFM examples and the raw HTML elements and
  attributes that are supported; everything else is shown as text.
- Mermaid diagrams in Mermaid 12.3 syntax, drawn through Merman 0.10: 23
  families, from flowcharts and sequence diagrams to Gantt charts, C4,
  mind maps and ZenUML, each named in the user guide.
- Citations and the bibliography read as the document's citation style
  writes them, author and date, numbers or notes, and a key that nothing
  defines stays in view. The preview reads a document in its own settings,
  the Standard profile too, and follows a change of them at once.
- What leads nowhere is shown as written in the theme's danger colour and
  says why when pointed at, to assistive technology too: an include that
  cannot be read in (a cycle with its chain of files), and a variable, a
  reference, a footnote or a heading link that nothing defines.
- The preview follows the text when you pause, and at least every three
  quarters of a second while you type, setting again only what changed, and
  scrolls with it; a book of 100,000 words keeps up without holding up the
  keys you type. A preview that takes more than a moment to set the text
  again says Updating….
- It also follows the files the text uses: a picture, an included file, a
  bibliography or a `.csl` style changed on disk is shown changed, and only
  what uses it is set again.
- The formatting toolbar steps aside in the reading view, where there is no
  text to format, and the preview stays where it was.
- Tables, code and formulas wider than the column scroll sideways by
  themselves.
- In the preview, web and mail links open, other kinds ask first and show
  where they lead, a task's box ticks the task in the text, a
  double click puts the caret at the same place in the text, and what you
  select copies as plain text and with its formatting. A double click in an
  included file's text opens that file at the place, and the outline lists
  included headings, each opening its file there.
- Preferences for the reading font, text size, line height and line length,
  for scrolling together, and for pictures from the web, which are shown
  only when you allow them, for one document or for all.
- The outline has a command and a key of its own, and a heading chosen from
  it is shown in the preview too.
- Distraction-free writing keeps the view: the text, the text beside its
  preview, or the preview alone. Focus mode and Vim keys are the text's.

### Exporting and printing

- Export to Markdown, HTML, PDF, DOCX and EPUB from the window
  (`Ctrl+Shift+E`), in the background with progress and Cancel, and print
  (`Ctrl+P`) through the desktop's print dialog. Before anything is written
  the dialog lists what the chosen format leaves out of the document.
- PDF: real pages of the size, margins and template chosen, numbered, with
  searchable text, links, an outline of the headings, `+++` page breaks and
  headings kept with their text; formulas and diagrams as vectors. The Export
  dialog shows the pages as they will come out, for a PDF and for printing,
  beside the format and its options where the window has room.
- DOCX: real styles, lists, tables with merged cells, links, footnotes or
  endnotes, pictures with their descriptions and the document's properties.
- EPUB 3: a file for each chapter, a table of contents, its images, formulas
  and diagrams as SVG, and metadata from the front matter.
- HTML: formulas as SVG and diagrams as SVG pictures, with images copied
  beside the page, written into it or linked.
- Export again repeats a document's last export, which parch keeps with the
  profile, options and renderers it was made with.
- Exports are written in one step, never over the document or a file it
  includes, and leave a file that was there as it was when they fail or are
  cancelled. A name given without its extension that leads to a file already
  there is asked about before that file is replaced.

### Keeping text safe

- Saves as you pause, replacing the file in one step and keeping its
  permissions, group, extended attributes and access lists.
- Crash recovery for text that has not reached its file yet.
- Version history with the reason each version was kept, named versions, and
  restore with undo. How many versions it keeps, for how many days and in
  how much space are preferences; a lower limit says how many versions it
  removes and asks first, and named versions are never removed.
- Conflicts when a file changes on disk under unsaved text, with a side by
  side review and Reload, Keep both, Overwrite or Save as.
- A file replaced by a link to another file is never read or written through
  that link unless you choose to follow it.
- A file another program renames, or moves into a folder parch watches, is
  followed, with its unsaved text, history and recent-file entry.

### Safe with documents from anywhere

- A document can only include files, and name bibliographies and styles,
  within its library or its own folder, and never makes parch read devices,
  system files or endless files. Deeply nested or self-multiplying
  bibliographies and styles are refused rather than read.
- Pictures from the web are never fetched from this machine or the local
  network, and are decoded only as the image they say they are.
- Bibliography links in HTML and EPUB exports are web, mail or DOI links only.
- Exports never write through a link at their destination.
- History, recovery, drafts, settings and the session are readable only by
  you.
- The math and diagram helpers run cut off from the network, with limits on
  memory and time.
- Very long words, very wide tables and very long documents keep the window
  responsive; a PDF export of one reports its progress and can be cancelled.

### Finding documents

- Find and replace in a document, with case, whole-word and regular
  expression matching; search across every library folder; go to a heading.
- Make, rename, move and trash files from the library, with links to them
  updated, and an outline of the document in front. A rename or move, with
  the links it updated, can be undone from its notice. Rename…, Move to…,
  Move to trash…, New document in folder… and Reopen as… act on the
  document in front from the menu and the command palette.
- Each document reopens where you were in it.
- A library of folders, drafts and recent files. Recent files can be removed
  one by one or cleared, and drafts deleted, each with Undo; a deleted
  draft's text stays in its history.
- Find a document by name across every library folder.
- Open files from the terminal, in the running window when there is one.
- The session reopens your windows and tabs where you left them.
- The library's edge can be dragged, or moved with the arrow keys; each
  window keeps its own width.
- The English user guide opens in a read-only tab from the menu or `F1`,
  and the package installs it in `/usr/share/doc/parch`.

### From the terminal

- `parch check` reports what is wrong in documents and the files they
  include, with file, line and column: includes, references, footnotes,
  citations and variables that lead nowhere, missing images, links to files
  or headings that are not there, formulas and Mermaid diagrams that cannot
  be drawn, and more. Its JSON names the RaTeX and Merman versions that drew
  them.
- `parch render` writes a document's HTML, plain text, outline or semantic
  tree, and `parch stats` counts its words, characters, reading time and
  headings.
- `parch export` writes a document as Markdown with its includes in place, a
  standalone HTML page, a PDF, a DOCX or an EPUB, as the window exports it,
  never over the document and never over another file unless asked.
- Every command needs no display, takes `--json`, and reads the Writer
  profile unless `--profile`, `--extension` or `--use-saved-settings` say
  otherwise. `--max-include-depth`, `--max-include-size` and
  `--max-includes` change the limits on includes.
- `parch render` and `parch export --to html` format citations and the
  bibliography in the document's citation style; `--bibliography` and
  `--csl` choose them, and `parch check` reports missing and duplicate keys,
  bibliography files that cannot be read, and styles that cannot be used.
- `parch skill install` installs a skill that teaches coding agents to use
  parch.
- `parch --screenshot PNG` draws the window off screen into a picture, set
  up by `--size`, `--view`, `--show`, `--library`, `--focus`, `--line`,
  `--scroll` and `--select`, without changing anything parch keeps;
  `--default-theme` shows Omarchy's default theme whatever the desktop's is.
- An example write-up with code, math, a table, a diagram, a footnote and
  citations, installed in `/usr/share/doc/parch/examples`.

### Installing

- Packages for x86_64 and ARM64 on each release, also under names without
  the version for `releases/latest/download`. The package is `parch-writer`,
  as the AUR's unrelated `parch` has that name; the command is `parch`.
