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

`Ctrl+B` shows or hides the library beside the text.

- **Drafts** lists your drafts. **Recent** lists the files you opened last.
- **Open folder** adds a folder. Its `.md`, `.markdown` and `.txt` files are
  listed beneath it and follow changes on disk.
- **Find a document** (`Ctrl+Shift+O`) filters every library folder by name,
  including folders you have not opened.

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
| `Ctrl+B` | Show or hide the library |
| `Ctrl+=` / `Ctrl+-` | Larger / smaller text |
| `Ctrl+0` | Theme text size |
| `F6` / `Shift+F6` | Move between the library, tabs, text and notices |
| `F10` | Open the menu |
| `Esc` | Dismiss a notice |

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
| `~/.config/parch/settings.json` | Settings, such as library folders and text size |

These follow `XDG_DATA_HOME`, `XDG_STATE_HOME` and `XDG_CONFIG_HOME` when they
are set.
