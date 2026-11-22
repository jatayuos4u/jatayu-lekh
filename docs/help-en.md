# Jatayu Lekh User Guide

Jatayu Lekh is a text editor built for JatayuOS. Use Text mode for everyday writing or Command mode for line numbers and quick access to editor commands. Press **F1** to return to this guide.

## 1. Create, open, and save a file

1. Press **Ctrl+N** for a new tab or **Ctrl+O** to open an existing file.
2. Type or paste your text. A dot on the tab means the document has unsaved edits.
3. Press **Ctrl+S**. For a new note, choose a folder and a name such as `meeting-notes.txt`.
4. Use **File → Save As** to save under a different name. Press **Ctrl+Shift+T** to reopen a recently closed tab.

Lekh starts with a blank tab by default. To return to your earlier tabs on launch, enable **Reopen previous tabs on startup** in Settings.

## 2. Switch modes or work side by side

- Click **Text** for a full-width editor. Click **Command** for line numbers, the command search bar, and the Split view button. **Ctrl+Shift+M** switches modes while keeping your document and edits.
- In Command mode, open two documents and choose **Split view**. Select the second document from the list.
- Press **Ctrl+K** to find editor commands and recent files. Press **F11** for Focus View and **Esc** to leave it.

## 3. Find, replace, and read the status bar

1. Press **Ctrl+F**, type a word, and press **Enter**. Use **F3** for the next match.
2. Press **Ctrl+H** to enter replacement text. Use **Replace** for one match or **Replace All** for the entire active document. Enable **Match case** when uppercase and lowercase should differ.
3. Use **Ctrl+G** to jump to a line. Use **View → Word Wrap** to control whether long lines wrap.

The status bar shows the cursor position and live counts for the active document. Character totals include spaces and line breaks. A blank line separates paragraphs. Wrapping a line on screen does not change the counts.

## 4. Preview Markdown and open HTML

- For Markdown, choose **Tools → Markdown Preview** to show a live preview beside the source. Choose it again to close the preview.
- For HTML, save your edits to an `.html` or `.htm` file and choose **Tools → Open HTML in Browser**. To see later edits, save again and refresh the browser.

## 5. Recover a draft or handle changes on disk

- Enable **Keep local drafts** in Settings. To find an unsaved draft, choose **File → Recover Draft** or press **Ctrl+Shift+R**. Select the draft, then use **Save As** if you want to keep a separate file.
- Use **File → Clear Recovery Data** to remove stored recovery data for this window and older snapshots. Open drafts may be stored again while recovery remains enabled.
- If another app edits the same file, choose **File → Review Disk Changes**. Use **Compare** to inspect the versions, **Reload from Disk** to use the disk version, **Keep Editing** to stay with your draft, or **Save a Copy** to preserve it separately.

## 6. Print or export a PDF

1. Press **Ctrl+P**. Choose the page size, orientation, and margins, then continue to the Print dialog.
2. Select a configured printer or choose **Print to File (PDF)**.
3. For PDF, use the output file picker to select a folder and filename. Click **Print** and open the saved PDF to check the result.

The **Advanced** printer tab depends on the selected printer. It may be unavailable when exporting a PDF.

## 7. Change language, appearance, and spelling

- In **Settings → Language**, choose **English** or **हिन्दी**, then restart Lekh to apply the interface language.
- Use **Settings → Theme** to choose the editor appearance. Use **Editor Font** for a different typeface or size.
- **English spellcheck** marks suspected misspellings. Right-click a marked word for suggestions. Autocorrect is optional and only changes a word when it has one suggestion.

## 8. Open a large log

Files larger than 16 MiB open in a read-only viewer. Use **Previous Chunk** and **Next Chunk** to move through the file, **Find Next** to search UTF-8 text, or **Follow End** to keep the newest log entries in view.

## JatayuOS

Jatayu Lekh was created by Anup Kumar Yadav for JatayuOS. Visit the [JatayuOS website](https://jatayuos.in/) for project information.
