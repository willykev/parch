# parch guide

parch is a Markdown writing app for Omarchy. It edits Markdown as plain text,
with every `#`, `*` and `[link](…)` in view, and keeps what you write safe
while you work.

This guide is also in parch: **User guide** in the menu, or `F1`, opens it
in a read-only tab. The package installs it as
`/usr/share/doc/parch/guide.md`. Beside it in the menu, **Markdown guide**
teaches the Markdown parch reads, with examples you can edit; see
[The Markdown guide](#the-markdown-guide).

## Documents and drafts

- **New draft** (`Ctrl+N`) opens an empty tab. A draft is kept in parch's own
  folder from the first keystroke, so it survives closing the window and
  restarting. It is listed under **Drafts** in the library.
- **Save** (`Ctrl+S`) on a draft asks where to put it, suggesting a name from
  its first heading or its first words. From then on it is an ordinary file.
- **Open** (`Ctrl+O`) uses your desktop's file dialog. Opening a file that is
  already open shows its tab instead of opening it twice.
- Opening a document replaces an empty draft you have not typed in.
- Run `parch new-note.md` with a name that does not exist yet to start a
  document that becomes that file when you first type.
- A file that is not valid UTF-8 opens read-only, so its bytes stay exactly as
  they are. **Reopen as…**, in the bar above the text, reads it in the
  encoding it was written in: Windows-1252, Latin-1, UTF-16 and every other
  your system knows, with a preview of the text in each. Once you convert
  it, you can edit it, and nothing is saved until you choose **Save as
  UTF-8**, which keeps the file's original bytes in its history first.
- parch opens files of up to 64 MiB, at paths of up to 4,095 bytes with every
  link followed. A larger file, or a longer path, is not opened, and parch
  says why.

parch keeps each file's line endings and byte-order mark. Text you paste gets
the document's line endings.

## Pasting, images and attachments

- **Paste** (`Ctrl+V`) turns rich text copied from a web page or a word
  processor into Markdown: headings, emphasis, links, lists and task lists,
  quotes, code and tables. Scripts, styles and hidden text are left out, and
  characters that would otherwise turn into Markdown are escaped. One undo
  takes the whole paste back. `Ctrl+Shift+V` always pastes plain text.
- A pasted image, such as a screenshot, is saved in the `assets` folder beside
  the document, named after the day as in `pasted-2026-11-16-1.png`, and
  linked where you pasted it. Pasting the same image again reuses its file.
  Images in the web page itself stay links to where they are online; nothing
  is downloaded.
- Dropping files on the text, or pasting files copied in a file manager,
  links them where they land. A file already in the document's folder or in
  the library is linked where it is; anything else is copied into `assets`,
  keeping its name, with `-2`, `-3` added when another file has it. Files
  over 50 MiB are copied only once you agree, and nothing you drop is moved.
- Dropping a Markdown document offers **Insert link** or **Include**, which
  puts its path on a line of its own so that its text is shown there.
- A draft keeps its assets in its own folder until you save it; they then go
  into `assets` beside the file.
- **Check links…** in the menu lists the links, images and includes whose
  files are not there, and the links to headings (`#a-heading`,
  `other.md#a-heading`) that the document, with what it includes, or that
  other document does not have.

## The Markdown guide

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

## Writing and formatting

Every formatting command writes plain Markdown into the text, and each is one
step for `Ctrl+Z` to take back. Find them under **Format** in the menu
(`F10`), on the toolbar under the tabs, in the command palette, or by their
keys.

- **Bold**, **Italic**, **Strikethrough**, **Highlight** (`==`), **Inline
  code**, **Inline math** (`$`) and **Comment** (`<!-- -->`) wrap the
  selection, or the text of each line in a selection over several lines.
  Used again on text that has them, they take the markers away. Without a
  selection they take away the markers around the caret, or put a pair
  there with the caret between them.
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
- **Table** inserts a two-column table; **Format table** lines up a table's
  pipes and padding by its alignment row without changing a cell, and keeps
  wide characters, escaped pipes and pipes in code in their place.
- **Thematic break** and **Page break** (`+++`) go on a line of their own,
  with a blank line before them.

While you type:

- `Enter` in a list item starts the next item: the same bullet, the next
  number, or an unchecked box. `Enter` on an empty item ends the list.
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

## Live formatting

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

## Preview

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

The preview follows the text a moment after you pause, and keeps up while you
type. Only what changed is set again, so it stays where you were reading. It
follows the files the text uses as well: a picture, an included file, a
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
  anything else is shown as the text it is and never run.

The preview reads the document as its [document settings](#document-settings)
say: a document in the Standard profile is previewed as Standard.

In the preview:

- It scrolls with the text beside it, each showing the same place. **Scroll
  sync** in the status line turns that off and on.
- A click on a link opens a web address in your browser and a Markdown file
  in a tab, at the heading the link names if it names one, and scrolls to a
  heading in the same document. A footnote's number goes to the note and
  the arrow after the note comes back.
  **Back in the preview** (`Alt+Left`) returns to where you were.
- A click on a task's box ticks it in the text; `Ctrl+Z` takes that back.
- A double click puts the caret at the same place in the text.
- Select text with the mouse, or with `Shift` and the arrow keys, and copy it
  with `Ctrl+C`, as plain text and with its formatting.
- `Up`, `Down`, `PageUp`, `PageDown`, `Space`, `Home` and `End` scroll.
  `F6` moves the keys between the library, the text and the preview.

## Commands and keys

The command palette (`Ctrl+Shift+P`) lists everything parch can do. Type a
few letters of a command's name, in order, to find it; the arrows choose and
`Enter` runs it.

**Keyboard shortcuts…** in the menu lists every command and its keys. Choose
one and press **Change…** (or `Enter`), then the keys you want. Keys that
type text, and keys kept for editing such as `Ctrl+C`, are refused; keys
another command has can be swapped. **Remove** leaves a command without keys,
**Reset** gives it its own back, and **Reset all** every command.

## Preferences

**Preferences…** (`Ctrl+,`) sets the writing font, any installed family or
the theme's; the text size; the line height; the longest line, in
characters; whether the Markdown is styled as you write; how headings are
written; whether the formatting toolbar shows; whether spelling is checked;
what focus mode keeps bright, and how strongly it dims the rest; and how many
versions history keeps, for how long and in how much space. A window too
narrow for the toolbar hides it until you ask for it with **Show toolbar**.
Every change applies at once in every window. Under **Keys**, **Edit with Vim
keys** turns on Vim keys. Under **Markdown and citations**, the Markdown
profile, extensions, citation style and bibliography for every document, as
[Document settings](#document-settings) describes them.

Under **Preview** it sets the reading font, which is a serif made for reading
unless you choose another; the preview's text size, two pixels larger than
the text's until you choose one; its line height and longest line; whether
the text and the preview scroll together; and whether pictures are fetched
from the web.

## Document settings

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

Each setting is the first of these that has one: the document's own
settings; its front matter (the bibliography and the citation style only);
the settings of the library folder that holds it; **Markdown and citations**
in **Preferences…**, for every document; and the defaults. Under each
setting the dialog says which it is.

Choose **This document** to change the document alone, or **Every document
in the library folder** to change every document in it; a setting of the
document itself still wins over its folder's. **Use the default** gives a
setting back to the next of these. A change applies at once to the preview,
live formatting, spelling, the outline, the word count and **Check links**. It
changes how the text is read, never the text: parch keeps these settings
itself, in `~/.local/share/parch/document-settings.json`, and they follow a
document when parch renames or moves it. A draft has settings of its own
from the start, and a library folder once it is saved in one.

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

## Vim keys

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
at once. Keys with `Ctrl` keep their meaning in parch in every mode: `Ctrl+S`
saves, `Ctrl+Z` undoes, `Ctrl+C`, `Ctrl+X` and `Ctrl+V` copy, cut and paste,
and so on. Vim's own `Ctrl` keys are only `Ctrl+R` (redo) and `Ctrl+[`.

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
character to the left. Outside Insert mode the editor takes no input method,
so that every key is a command.

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

## Saving

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

## Recovery

Text that is not in its file yet is copied to recovery within half a second
of a pause. If parch or the computer stops, the next launch brings the text
back and says so. Recovery for a document is cleared once its file holds the
text.

## History

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
  **History** in **Preferences…** you can change all three. A lower limit
  removes the older versions past it at once, but never a version you named.
- If named versions alone fill that space, history pauses and the notice bar
  says so. Saving carries on.

## Changes on disk

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

## Tabs and windows

- `Ctrl+Tab` / `Ctrl+Shift+Tab`, or `Ctrl+PageDown` / `Ctrl+PageUp`, move
  between tabs. `Ctrl+W` closes one.
- `Ctrl+Shift+N` opens a new window. **New view** in the menu shows the same
  document in another tab; edits appear in both.
- Closing a tab saves it first. If it cannot be saved, parch asks before
  closing.
- `Ctrl+Q` saves everything and quits. The next launch reopens your windows
  and tabs where you left them.

## The library

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
  in front; choosing one goes there, in the text and in the preview.
  `Ctrl+Alt+O` shows it with the keys on the headings, and hides it.
- A folder's menu (right click, or the `Menu` key) makes a **New document…**
  or **New folder…** in it. A document's menu can **Rename…** it (`F2`),
  **Move to…** another library folder (or drag it there), and **Move to
  trash…** (`Delete`), from where the desktop's trash can bring it back.
- When a renamed or moved document has links, images, includes or reference
  definitions leading to it in other documents of the library, or relative
  ones of its own, parch lists them and offers to update them. Open documents are updated in their tabs, as one step for `Ctrl+Z`.
  A moved document keeps its tab, its history and any unsaved text.

## Finding text

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

Each document opens again where you were in it last time.

## Appearance

parch draws everything from your Omarchy theme and follows it as you switch
themes, light or dark, along with the theme's font. `Ctrl+=` and `Ctrl+-`
change the text size, and `Ctrl+0` returns to the theme's size.

## Spelling and focus

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

## Keys

| Key | Action |
| --- | --- |
| `Ctrl+N` | New draft |
| `Ctrl+Shift+N` | New window |
| `Ctrl+O` | Open a file |
| `Ctrl+Shift+O` | Find a document |
| `Ctrl+F` / `Ctrl+H` | Find / find and replace |
| `F3` / `Shift+F3` | Next / previous match |
| `Ctrl+Shift+F` | Search the library |
| `Ctrl+R` | Go to heading |
| `Ctrl+S` | Save now |
| `Ctrl+Shift+S` | Save as |
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

  open             Open the files that follow (optional; bare files also open)
  --theme DIR      Read theme tokens from DIR instead of the Omarchy theme
  --benchmark OUT  Measure the window on a copy of one FILE; write JSON to OUT
  --version        Print the version and exit
  --help           Print this help and exit

Commands that need no display (parch help COMMAND for its options):
  check FILE...    Report what is wrong in documents, with file, line and column
  render FILE      Write a document's HTML, plain text, outline or semantic tree
  export FILE      Write a document as Markdown with its includes, or as an HTML page
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
parch render notes.md --to outline   # html, text, outline or json-tree
parch export book.md -o book.html    # A page of its own
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
  not count as problems.
- **render** writes the HTML fragment the preview and exports are made from
  (`html`, the default), the text a reader sees (`text`), the headings
  indented by level (`outline`), or the whole semantic tree with source ranges
  (`json-tree`). Relative links and images lead from the folder the HTML is
  written to, or from the document's.
- **export** writes `md`, the document as one file with each include in
  place (an included file's front matter left out and its variables written
  out, its footnote and link labels kept apart from the document's, and its
  paths written from the new file's folder), or `html`, a standalone page with
  typography of its own that does not follow the desktop theme (`--title`
  sets its title). Citations and the bibliography are written as the citation
  style has them. Math and Mermaid diagrams are written as their source in
  the page, and its notes say so. The format follows `-o`'s name unless `--to`
  gives one. PDF, DOCX and EPUB cannot be exported from the terminal yet.
- **stats** counts the words, characters (with and without spaces) and
  headings a reader reads, and the reading time at 200 words a minute.

An export never writes over the document or a file it includes, replaces a
file that is there only with `--force`, and is not made at all when a file
the document includes cannot be read. `--quiet` prints nothing but errors.
`render -o` follows the same rules.

### The dialect

Every command reads the Writer profile with all of its extensions, whatever
the window was last set to, so a command gives the same result on any
machine. These options change it, the highest first:

- `--profile writer|standard`, `--extension NAME` and `--no-extension NAME`
  (given again for more); `parch help check` lists the extensions.
- `--library DIR`: the folder `/rooted` includes are found in.
- `--bibliography FILE` (given again for more) and `--csl STYLE`: the
  bibliography and the citation style, a bundled one or a `.csl` file.
- `--use-saved-settings`: read the document as the window reads it, with the
  [document settings](#document-settings) kept for the document, its library
  folder and every document, and the library folder for `/rooted` includes.
  What the options say still wins.

The front matter's `bibliography:` and `csl:` choose the bibliography and the
style unless the options, or with `--use-saved-settings` the document's own
settings, choose them. JSON results report the profile, the extensions, the
bibliography and the style, where each came from, the dialect version, and
the version of what drew the result.

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Done, and nothing wrong |
| 1 | The command line is wrong |
| 2 | `check` found problems; `export` was given a document whose includes cannot all be read |
| 3 | A file could not be read or written, or is there and `--force` was not given |
| 4 | The export could not be made, as for a format that is not available yet |

### For coding agents

`parch skill` prints a skill that tells coding agents how to write, check,
render and export your documents with these commands. `parch skill install`
installs it as `~/.agents/skills/parch/SKILL.md`, with a link to it from
`~/.claude/skills/parch` when `~/.claude` exists. Running it again updates
it; other skills, and a file or link there that is not parch's, are left
alone.

## Files parch keeps

| Path | Contents |
| --- | --- |
| `~/.local/share/parch/drafts/` | Drafts, one folder each |
| `~/.local/share/parch/history/` | Version history, one folder per document |
| `~/.local/share/parch/documents.json` | Which history belongs to which file |
| `~/.local/share/parch/dictionary.txt` | Words added to the spelling dictionary |
| `~/.local/share/parch/document-settings.json` | Document settings, by document, library folder and for every document |
| `~/.local/state/parch/recovery/` | Text not yet in its file |
| `~/.local/state/parch/session.json` | Open windows and tabs |
| `~/.local/state/parch/recent.json` | Recent files |
| `~/.local/state/parch/positions.json` | Where you were in each document |
| `~/.config/parch/settings.json` | Settings, such as library folders, text size and your own keys |
| `~/.cache/parch/render/` | Formulas and diagrams already drawn, at most 128 MiB |

These follow `XDG_DATA_HOME`, `XDG_STATE_HOME`, `XDG_CONFIG_HOME` and
`XDG_CACHE_HOME` when they are set.
