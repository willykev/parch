# Changelog

## 0.1.0 — unreleased

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
- Rich text pastes as clean Markdown; `Ctrl+Shift+V` pastes plain text.
- Pasted images and dropped files go into an `assets` folder beside the
  document; a dropped Markdown document can be linked or included.
- Check links lists the links, images and includes whose files are missing,
  and the links to headings that are not there.
- Formatting commands that write and toggle Markdown, each one undo step, from
  the menu, a formatting toolbar, a command palette and their keys.
- Lists that continue on Enter and indent with Tab, table cells reached with
  Tab, table formatting, and Shift+Enter for a hard line break.
- Keys you can change, with refusals for keys that type and swapping for keys
  taken.
- English spell checking as you write, with wavy underlines, suggestions, a
  personal dictionary and Ignore, from a right click or the `Menu` key.
- Focus mode, which dims all but the sentence or paragraph being written, and
  distraction-free writing, which shows the text alone.
- Preferences for the writing font, text size, line height, line length,
  live formatting, heading syntax, toolbar, spelling and focus mode, applied
  at once in every window.
- A word count and reading time for the document or the selection, of the
  text a reader sees.
- Document settings for each document, kept by parch and never in the file:
  the Markdown profile (Writer-compatible or Standard), its extensions, the
  citation style, the bibliography and the library root, with defaults for a
  library folder and, in Preferences, for every document. The dialog shows
  where each setting comes from, and live formatting, spelling, the outline,
  the word count and Check links read the document as they say.
- Citations and bibliographies: Writer `[p. 23][#key]` and Pandoc
  `[@key, p. 23]` citations, `[-@key]` without the author, Writer
  bibliography entries written in the document, and BibTeX, BibLaTeX, CSL
  JSON and CSL YAML files, chosen in Document settings or with
  `bibliography:` and `csl:` in the front matter. Seven bundled CSL styles
  (APA, Chicago author-date and notes, IEEE, MLA, Harvard, Vancouver) or a
  `.csl` file of your own; note styles put citations in footnotes.
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
- Citations and the bibliography read as the document's citation style
  writes them, author and date, numbers or notes, and a key that nothing
  defines stays in view. The preview reads a document in its own settings,
  the Standard profile too, and follows a change of them at once.
- The preview follows the text as you type, setting again only what changed,
  and scrolls with it; a book of 100,000 words keeps up.
- Tables, code and formulas wider than the column scroll sideways by
  themselves.
- In the preview, links open, a task's box ticks the task in the text, a
  double click puts the caret at the same place in the text, and what you
  select copies as plain text and with its formatting.
- Preferences for the reading font, text size, line height and line length,
  for scrolling together, and for fetching pictures from the web.
- The outline has a command and a key of its own, and a heading chosen from
  it is shown in the preview too.
- Distraction-free writing keeps the view: the text, the text beside its
  preview, or the preview alone. Focus mode and Vim keys are the text's.

### Keeping text safe

- Saves as you pause, replacing the file in one step and keeping its
  permissions, group, extended attributes and access lists.
- Crash recovery for text that has not reached its file yet.
- Version history with the reason each version was kept, named versions, and
  restore with undo.
- Conflicts when a file changes on disk under unsaved text, with a side by
  side review and Reload, Keep both, Overwrite or Save as.
- A file replaced by a link to another file is never read or written through
  that link unless you choose to follow it.

### Finding documents

- Find and replace in a document, with case, whole-word and regular
  expression matching; search across every library folder; go to a heading.
- Make, rename, move and trash files from the library, with links to them
  updated, and an outline of the document in front.
- Each document reopens where you were in it.
- A library of folders, drafts and recent files.
- Find a document by name across every library folder.
- Open files from the terminal, in the running window when there is one.
- The session reopens your windows and tabs where you left them.

### From the terminal

- `parch check` reports what is wrong in documents and the files they
  include, with file, line and column: includes, references, footnotes,
  citations and variables that lead nowhere, missing images, links to files
  or headings that are not there, and more.
- `parch render` writes a document's HTML, plain text, outline or semantic
  tree, and `parch stats` counts its words, characters, reading time and
  headings.
- `parch export` writes a document as one Markdown file with its includes in
  place, or as a standalone HTML page, never over the document and never
  over another file unless asked. PDF, DOCX and EPUB are not available from
  the terminal yet.
- Every command needs no display, takes `--json`, and reads the Writer
  profile unless `--profile`, `--extension` or `--use-saved-settings` say
  otherwise.
- `parch render` and `parch export --to html` format citations and the
  bibliography in the document's citation style; `--bibliography` and
  `--csl` choose them, and `parch check` reports missing and duplicate keys,
  bibliography files that cannot be read, and styles that cannot be used.
- `parch skill install` installs a skill that teaches coding agents to use
  parch.
