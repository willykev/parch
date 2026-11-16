# parch guide

parch is a Markdown writing app for Omarchy. It edits Markdown as plain text,
with every `#`, `*` and `[link](…)` in view, and keeps what you write safe
while you work.

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
  they are.

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
  files are not there.
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
how many minutes they take to read at 200 words a minute. Chinese and
Japanese count each character as a word.

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
characters; how headings are written; and whether the formatting toolbar
shows. A window too narrow for the toolbar hides it until you ask for it with
**Show toolbar**. Every change applies at once in every window.

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
- **Name this version** keeps a version until you delete it. Unnamed versions
  are kept for 30 days, at most 100 per document, within 1 GiB in all.
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

`F9` shows or hides the library beside the text.

- **Drafts** lists your drafts. **Recent** lists the files you opened last.
- **Open folder** adds a folder. Its `.md`, `.markdown` and `.txt` files are
  listed beneath it and follow changes on disk.
- **Find a document** (`Ctrl+Shift+O`) filters every library folder by name,
  including folders you have not opened.
- **Outline**, at the foot of the library, lists the headings of the document
  in front; choosing one goes there.
- A folder's menu (right click, or the `Menu` key) makes a **New document…**
  or **New folder…** in it. A document's menu can **Rename…** it (`F2`),
  **Move to…** another library folder (or drag it there), and **Move to
  trash…** (`Delete`), from where the desktop's trash can bring it back.
- When a renamed or moved document has links to it in other documents of the
  library, or relative links of its own, parch lists them and offers to update
  them. Open documents are updated in their tabs, as one step for `Ctrl+Z`.
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
| `Ctrl+=` / `Ctrl+-` | Larger / smaller text |
| `Ctrl+0` | Theme text size |
| `F6` / `Shift+F6` | Move between the library, tabs, text and notices |
| `F10` | Open the menu |
| `Esc` | Dismiss a notice |
| `Ctrl+Shift+P` | Command palette |
| `Ctrl+,` | Preferences |
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

Every key can be changed with **Keyboard shortcuts…**.

## Command line

```text
Usage: parch [open] [--theme DIR] [--benchmark OUT] [FILE...]

  open             Open the files that follow (optional; bare files also open)
  --theme DIR      Read theme tokens from DIR instead of the Omarchy theme
  --benchmark OUT  Measure the window on a copy of one FILE; write JSON to OUT
  --version        Print the version and exit
  --help           Print this help and exit
```

Files and folders given to `parch` while it is running open in the running
window. A folder is added to the library.

## Files parch keeps

| Path | Contents |
| --- | --- |
| `~/.local/share/parch/drafts/` | Drafts, one folder each |
| `~/.local/share/parch/history/` | Version history, one folder per document |
| `~/.local/share/parch/documents.json` | Which history belongs to which file |
| `~/.local/state/parch/recovery/` | Text not yet in its file |
| `~/.local/state/parch/session.json` | Open windows and tabs |
| `~/.local/state/parch/recent.json` | Recent files |
| `~/.local/state/parch/positions.json` | Where you were in each document |
| `~/.config/parch/settings.json` | Settings, such as library folders, text size and your own keys |

These follow `XDG_DATA_HOME`, `XDG_STATE_HOME` and `XDG_CONFIG_HOME` when they
are set.
