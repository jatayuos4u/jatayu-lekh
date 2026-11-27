#include "projectlinks.h"
#include <QtCore>
#include <QtWidgets>
#include <QtPrintSupport>
#include <algorithm>
#include <memory>
#include <utility>
#include <iconv.h>
#include <optional>
#include "regularfile.h"
#include "diff.h"
#include "logviewer.h"
#include "spelling.h"
#include "hindi.h"

struct Format {
    QString encoding = "UTF-8";
    QString ending = "LF";
    bool bom = false;
};

static std::optional<QByteArray> convertEncoding(const QByteArray &bytes, const char *to, const char *from)
{
    iconv_t converter = iconv_open(to, from);
    if (converter == reinterpret_cast<iconv_t>(-1))
        return std::nullopt;
    QByteArray output(bytes.size() * 4 + 16, Qt::Uninitialized);
    auto input = const_cast<char *>(bytes.constData());
    auto out = output.data();
    size_t inputLeft = bytes.size();
    size_t outputLeft = output.size();
    const auto result = iconv(converter, &input, &inputLeft, &out, &outputLeft);
    iconv_close(converter);
    if (result == size_t(-1)) return std::nullopt;
    output.resize(output.size() - outputLeft);
    return output;
}

static std::optional<QByteArray> encode(const QString &source, const Format &format)
{
    QString text = source;
    if (format.ending == "CRLF")
        text.replace("\n", "\r\n");
    if (format.ending == "CR")
        text.replace("\n", "\r");
    if (format.encoding == "UTF-8")
        return (format.bom ? QByteArray::fromHex("efbbbf") : QByteArray()) + text.toUtf8();
    if (format.encoding == "Windows-1252")
        return convertEncoding(text.toUtf8(), "WINDOWS-1252", "UTF-8");
    const bool littleEndian = format.encoding == "UTF-16LE";
    QStringEncoder encoder(littleEndian ? QStringConverter::Utf16LE : QStringConverter::Utf16BE,
                           QStringConverter::Flag::WriteBom);
    return encoder(text);
}

static std::optional<QString> decode(QByteArray bytes, Format &format, bool legacy = false)
{
    QStringConverter::Encoding encoding = QStringConverter::Utf8;
    if (legacy) {
        const auto converted = convertEncoding(bytes, "UTF-8", "WINDOWS-1252");
        if (!converted) return std::nullopt;
        bytes = *converted;
        format.encoding = "Windows-1252";
    } else if (bytes.startsWith(QByteArray::fromHex("efbbbf"))) {
        format.bom = true;
        bytes.remove(0, 3);
    } else if (bytes.startsWith(QByteArray::fromHex("fffe"))) {
        format.encoding = "UTF-16LE";
        encoding = QStringConverter::Utf16LE;
        bytes.remove(0, 2);
    } else if (bytes.startsWith(QByteArray::fromHex("feff"))) {
        format.encoding = "UTF-16BE";
        encoding = QStringConverter::Utf16BE;
        bytes.remove(0, 2);
    }
    QStringDecoder decoder(encoding);
    QString text = decoder(bytes);
    if (decoder.hasError() || text.contains(QChar::Null))
        return std::nullopt;
    const auto firstNewline = text.indexOf(QRegularExpression("[\\r\\n]"));
    if (firstNewline >= 0 && text[firstNewline] == '\r')
        format.ending = text.mid(firstNewline, 2) == "\r\n" ? "CRLF" : "CR";
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');
    return text;
}

static constexpr qint64 textLimit = 16 * 1024 * 1024;
static constexpr qint64 recoveryLimit = 128 * 1024 * 1024;
static constexpr qint64 archiveLimit = 16 * 1024 * 1024;

static std::optional<QByteArray> readRegular(const QString &path, qint64 limit = textLimit)
{
    QFile file(path);
    if (!openRegular(file) || file.size() > limit) return std::nullopt;
    auto bytes = file.read(limit + 1);
    if (bytes.size() > limit || file.error() != QFileDevice::NoError) return std::nullopt;
    return bytes;
}

static bool writeFile(const QString &path, const QByteArray &bytes, bool privateFile = false,
                      const std::function<bool()> &beforeCommit = {})
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (privateFile && !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return false;
    return file.write(bytes) == bytes.size() && (!beforeCommit || beforeCommit()) && file.commit();
}

struct TextCounts {
    qsizetype words = 0;
    qsizetype characters = 0;
    qsizetype paragraphs = 0;
};

static TextCounts countText(const QString &text)
{
    TextCounts counts;
    QTextBoundaryFinder words(QTextBoundaryFinder::Word, text);
    while (words.position() >= 0) {
        if (words.boundaryReasons().testFlag(QTextBoundaryFinder::StartOfItem))
            ++counts.words;
        words.toNextBoundary();
    }

    QTextBoundaryFinder characters(QTextBoundaryFinder::Grapheme, text);
    while (characters.toNextBoundary() >= 0)
        ++counts.characters;

    bool inParagraph = false;
    const QStringView view(text);
    qsizetype start = 0;
    while (start < view.size()) {
        const qsizetype newline = view.indexOf(u'\n', start);
        const qsizetype end = newline < 0 ? view.size() : newline;
        const bool hasText = !view.mid(start, end - start).trimmed().isEmpty();
        if (hasText && !inParagraph)
            ++counts.paragraphs;
        inParagraph = hasText;
        start = end + 1;
    }
    return counts;
}

class MarkdownPreview : public QTextBrowser {
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

class Editor : public QPlainTextEdit {
public:
    Editor()
    {
        setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
        document()->setDocumentMargin(17);
        gutter = new QWidget(this);
        gutter->hide();
        connect(this, &QPlainTextEdit::updateRequest, this, [this] { gutter->update(); });
        gutter->installEventFilter(this);
    }

    std::function<void(QTextCursor)> correctWord;

    void showNumbers(bool visible)
    {
        gutter->setVisible(visible);
        setViewportMargins(visible ? 48 : 0, 0, 0, 0);
        gutter->setGeometry(0, 0, 48, height());
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (correctWord && !isReadOnly() && !textCursor().hasSelection() &&
            (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::ShiftModifier) &&
            (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            auto word = textCursor();
            if (word.positionInBlock() > 0) {
                const auto before = word.block().text().left(word.positionInBlock());
                static const QRegularExpression ending("[A-Za-z][A-Za-z']{2,60}$");
                const auto candidate = ending.match(before);
                if (candidate.hasMatch()) {
                    word.setPosition(word.position() - candidate.capturedLength(), QTextCursor::KeepAnchor);
                    correctWord(word);
                }
            }
        }
        QPlainTextEdit::keyPressEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QPlainTextEdit::resizeEvent(event);
        gutter->setGeometry(0, 0, 48, height());
    }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object != gutter || event->type() != QEvent::Paint)
            return QPlainTextEdit::eventFilter(object, event);
        QPainter painter(gutter);
        painter.fillRect(gutter->rect(), palette().alternateBase());
        painter.setPen(palette().text().color());
        for (auto block = firstVisibleBlock(); block.isValid(); block = block.next()) {
            const int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
            if (top >= gutter->height())
                break;
            if (block.isVisible())
                painter.drawText(0, top, 39, fontMetrics().height(), Qt::AlignRight,
                                 QString::number(block.blockNumber() + 1));
        }
        return true;
    }

private:
    QWidget *gutter = nullptr;
};

struct Document {
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString path;
    Format format;
    QByteArray diskVersion;
    bool diskConflict = false;
    QWidget *page;
    QSplitter *views;
    Editor *editor;
    QPointer<QPlainTextEdit> comparison;
    QPointer<QWidget> preview;
};

class Window : public QMainWindow {
public:
    Window()
    {
        systemPalette = qApp->palette();
        spellEnabled = settings.value("spellcheck", true).toBool();
        autocorrectEnabled = settings.value("autocorrect", false).toBool();
        resize(1120, 700);
        setWindowIcon(QIcon(":/in.jatayuos.Lekh.svg"));
        setWindowTitle("Jatayu Lekh");
        setAcceptDrops(true);

        const QString data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        dataDirectory = data;
        QDir().mkpath(data);
        fileWatcher = new QFileSystemWatcher(this);
        connect(fileWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) { diskChanged(path); });
        connect(fileWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &directory) {
            const auto watched = fileWatcher->files();
            for (const auto &item : documents) {
                if (!item->path.isEmpty() && QFileInfo(item->path).absolutePath() == directory &&
                    !watched.contains(item->path))
                    diskChanged(item->path);
            }
        });
        const bool privateDirectory = QFileInfo(data).isDir() && !QFileInfo(data).isSymLink() &&
            QFile::setPermissions(data, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        for (int slot = 1; privateDirectory; ++slot) {
            const QString prefix = data + QString("/window-%1").arg(slot);
            sessionLock = std::make_unique<QLockFile>(prefix + ".lock");
            if (sessionLock->tryLock(0)) {
                statePath = prefix + ".json";
                break;
            }
            if (sessionLock->error() != QLockFile::LockFailedError)
                break;
        }

        auto root = new QWidget;
        auto layout = new QVBoxLayout(root);
        layout->setContentsMargins(12, 8, 12, 8);
        layout->setSpacing(7);
        setCentralWidget(root);
        auto toolbar = new QHBoxLayout;
        layout->addLayout(toolbar);
        auto file = menuBar()->addMenu(tr("&File"));
        auto edit = menuBar()->addMenu(tr("&Edit"));
        auto view = menuBar()->addMenu(tr("&View"));
        auto tools = menuBar()->addMenu(tr("&Tools"));
        auto help = menuBar()->addMenu(tr("&Help"));
        auto newAction = action(file, tr("New Tab"), QKeySequence::New, [this] { addDocument(); });
        action(file, tr("New Window"), QKeySequence("Ctrl+Shift+N"), [] {
            QProcess::startDetached(QCoreApplication::applicationFilePath(), QStringList{});
        });
        auto openAction = action(file, tr("Open…"), QKeySequence::Open, [this] { openDialog(); });
        auto saveAction = action(file, tr("Save"), QKeySequence::Save, [this] { if (auto d = current()) save(d); });
        action(file, tr("Save As…"), QKeySequence::SaveAs, [this] { if (auto d = current()) save(d, true); });
        action(file, tr("Close Tab"), QKeySequence::Close, [this] { closeTab(tabs->currentIndex()); });
        action(file, tr("Recover Draft…"), QKeySequence("Ctrl+Shift+R"), [this] { recoverDraft(); });
        action(file, tr("Clear Recovery Data…"), {}, [this] { clearRecovery(); });
        action(file, tr("Reopen Closed Tab"), QKeySequence("Ctrl+Shift+T"), [this] { reopen(); });
        action(file, tr("Review Disk Changes…"), {}, [this] { if (auto d = current()) reviewDisk(d); });
        file->addSeparator();
        action(file, tr("Page Setup & Print…"), QKeySequence::Print, [this] { printDocument(); });
        action(file, tr("Quit"), QKeySequence::Quit, [this] { close(); });

        for (auto item : {newAction, openAction, saveAction}) {
            auto button = new QToolButton;
            button->setDefaultAction(item);
            toolbar->addWidget(button);
        }
        toolbar->addStretch();
        textMode = new QPushButton(tr("Text"));
        commandMode = new QPushButton(tr("Command"));
        auto modes = new QButtonGroup(this);
        for (auto button : {textMode, commandMode}) {
            button->setCheckable(true);
            modes->addButton(button);
            toolbar->addWidget(button);
        }
        toolbar->addStretch();
        auto settingsButton = new QPushButton(tr("Settings"));
        toolbar->addWidget(settingsButton);
        connect(settingsButton, &QPushButton::clicked, this, [this] { settingsDialog(); });

        commandRow = new QWidget;
        auto commands = new QHBoxLayout(commandRow);
        commands->setContentsMargins(0, 0, 0, 0);
        auto search = new QPushButton(tr("Search files or commands     Ctrl+K"));
        search->setStyleSheet("text-align:left;padding-left:12px");
        commands->addWidget(search, 1);
        auto split = new QPushButton(tr("Split view"));
        commands->addWidget(split);
        layout->addWidget(commandRow);
        connect(search, &QPushButton::clicked, this, [this] { paletteDialog(); });
        connect(split, &QPushButton::clicked, this, [this] { toggleSplit(); });

        findRow = new QWidget;
        auto findLayout = new QHBoxLayout(findRow);
        findLayout->setContentsMargins(0, 0, 0, 0);
        findField = new QLineEdit;
        replaceField = new QLineEdit;
        findField->setPlaceholderText(tr("Find"));
        replaceField->setPlaceholderText(tr("Replace"));
        findLayout->addWidget(findField);
        findLayout->addWidget(replaceField);
        matchCase = new QCheckBox(tr("Match case"));
        findLayout->addWidget(matchCase);
        auto barButton = [this, findLayout](const QString &name, auto callback) {
            auto button = new QPushButton(name);
            findLayout->addWidget(button);
            connect(button, &QPushButton::clicked, this, callback);
        };
        barButton(tr("Previous"), [this] { findNext(true); });
        barButton(tr("Next"), [this] { findNext(); });
        barButton(tr("Replace"), [this] { replace(false); });
        barButton(tr("Replace All"), [this] { replace(true); });
        barButton("×", [this] { findRow->hide(); if (auto d = current()) d->editor->setFocus(); });
        connect(findField, &QLineEdit::returnPressed, this, [this] { findNext(); });
        layout->addWidget(findRow);
        findRow->hide();

        auto area = new QSplitter;
        layout->addWidget(area, 1);
        tabs = new QTabWidget;
        tabs->setTabsClosable(true);
        tabs->setMovable(true);
        area->addWidget(tabs);
        auto plus = new QToolButton;
        plus->setText("+");
        plus->setToolTip(tr("New Tab"));
        tabs->setCornerWidget(plus);
        connect(plus, &QToolButton::clicked, this, [this] { addDocument(); });
        connect(tabs, &QTabWidget::tabCloseRequested, this, [this](int index) { closeTab(index); });
        connect(tabs, &QTabWidget::currentChanged, this, [this] { refresh(); });

        position = new QLabel;
        details = new QLabel;
        statistics = new QLabel;
        statistics->setToolTip(tr("Characters include spaces and line breaks. Blank lines separate paragraphs."));
        statisticsTimer = new QTimer(this);
        statisticsTimer->setSingleShot(true);
        statisticsTimer->setInterval(150);
        connect(statisticsTimer, &QTimer::timeout, this, [this] { updateStatistics(); });
        statusBar()->addWidget(position);
        statusBar()->addWidget(statistics, 1);
        statusBar()->addPermanentWidget(details);
        connect(textMode, &QPushButton::clicked, this, [this] { setMode(false); });
        connect(commandMode, &QPushButton::clicked, this, [this] { setMode(true); });

        action(edit, tr("Undo"), QKeySequence::Undo, [this] { if (auto e = activeEditor()) e->undo(); });
        action(edit, tr("Redo"), QKeySequence::Redo, [this] { if (auto e = activeEditor()) e->redo(); });
        action(edit, tr("Cut"), QKeySequence::Cut, [this] { if (auto e = activeEditor()) e->cut(); });
        action(edit, tr("Copy"), QKeySequence::Copy, [this] { if (auto e = activeEditor()) e->copy(); });
        action(edit, tr("Paste"), QKeySequence::Paste, [this] { if (auto e = activeEditor()) e->paste(); });
        action(edit, tr("Select All"), QKeySequence::SelectAll, [this] { if (auto e = activeEditor()) e->selectAll(); });
        action(edit, tr("Find"), QKeySequence::Find, [this] { showFind(false); });
        action(edit, tr("Replace"), QKeySequence("Ctrl+H"), [this] { showFind(true); });
        action(edit, tr("Find Next"), QKeySequence::FindNext, [this] { findNext(); });
        action(edit, tr("Go to Line"), QKeySequence("Ctrl+G"), [this] { goToLine(); });
        action(edit, tr("Insert Date & Time"), QKeySequence("F5"), [this] {
            if (auto d = current()) d->editor->insertPlainText(QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat));
        });
        action(view, tr("Switch Mode"), QKeySequence("Ctrl+Shift+M"), [this] { setMode(!command); });
        action(view, tr("Focus View"), QKeySequence("F11"), [this] { toggleFocus(); });
        action(view, tr("Split View"), QKeySequence("Ctrl+Shift+2"), [this] { toggleSplit(); });
        action(view, tr("Word Wrap"), {}, [this] { wrap = !wrap; applyEditorSettings(); });
        action(view, tr("Zoom In"), QKeySequence::ZoomIn, [this] { adjustZoom(1); });
        action(view, tr("Zoom Out"), QKeySequence::ZoomOut, [this] { adjustZoom(-1); });
        action(view, tr("Reset Zoom"), QKeySequence("Ctrl+0"), [this] { zoom = 0; applyEditorSettings(); });
        addCommands(tools);
        action(help, tr("User Guide"), QKeySequence::HelpContents, [this] { showGuide(); });
        addHelpLink(help, tr("JatayuOS Website"), jatayuWebsite());
        if (*projectRepository)
            addHelpLink(help, tr("Lekh Source on GitHub"), QUrl(QString::fromUtf8(projectRepository)));
        else
            addHelpLink(help, tr("JatayuOS on GitHub"), QUrl("https://github.com/jatayuos4u"));
        if (*projectLicense)
            addHelpLink(help, tr("Lekh License"), QUrl(QString::fromUtf8(projectLicense)));
        else
            action(help, tr("Lekh License"), {}, [this] {
                QMessageBox::information(this, tr("Lekh License"),
                    tr("A project license link has not been configured for this build."));
            });
        auto legal = help->addMenu(tr("GitHub Legal"));
        addHelpLink(legal, tr("GitHub Terms of Service"),
                    QUrl("https://docs.github.com/en/site-policy/github-terms/github-terms-of-service"));
        addHelpLink(legal, tr("GitHub Privacy Statement"),
                    QUrl("https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement"));
        help->addSeparator();
        action(help, tr("About Jatayu Lekh"), {}, [this] {
            QMessageBox about(this);
            about.setWindowTitle(tr("About Jatayu Lekh"));
            about.setIconPixmap(windowIcon().pixmap(48, 48));
            about.setText(tr("Jatayu Lekh %1").arg(QCoreApplication::applicationVersion()));
            about.setInformativeText(tr("A text editor built for JatayuOS.\nCreator: Anup Kumar Yadav"));
            about.setStandardButtons(QMessageBox::Close);
            auto website = about.addButton(tr("JatayuOS Website"), QMessageBox::ActionRole);
            about.exec();
            if (about.clickedButton() == website) openHelpUrl(jatayuWebsite());
        });
        focusShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        connect(focusShortcut, &QShortcut::activated, this, [this] {
            if (focused) toggleFocus();
            else if (findRow->isVisible()) findRow->hide();
        });

        baseFont = settings.value("font", QFontDatabase::systemFont(QFontDatabase::FixedFont)).value<QFont>();
        theme = settings.value("theme", "Design").toString();
        sessionEnabled = settings.value("session", true).toBool();
        setMode(settings.value("command", false).toBool());
        recent = settings.value("recent").toStringList();
        if (sessionEnabled && !QCoreApplication::arguments().contains("--smoke-test")) {
            loadSession(settings.value("restoreTabsOnStartup", false).toBool());
        }
        if (tabs->count() == 0)
            addDocument();
        auto timer = new QTimer(this);
        timer->start(3000);
        connect(timer, &QTimer::timeout, this, [this] { flushSession(); });
    }

    void openPath(const QString &path)
    {
        const QFileInfo info(path);
        const QString resolved = info.canonicalFilePath();
        if (!resolved.isEmpty()) {
            for (auto &item : documents) {
                if (item->path == resolved) {
                    tabs->setCurrentWidget(item->page);
                    return;
                }
            }
        }
        QFile file(path);
        if (!openRegular(file)) {
            QMessageBox::warning(this, tr("Open File"), tr("Could not open this file. Select a readable regular file."));
            return;
        }
        if (file.size() > textLimit) {
            (new LogViewer(path, this))->show();
            return;
        }
        const auto bytes = file.read(textLimit + 1);
        Format format;
        auto content = decode(bytes, format);
        if (!content && bytes.size() <= textLimit && file.error() == QFileDevice::NoError && !bytes.contains(char(0))) {
            Format legacyFormat;
            auto legacyText = decode(bytes, legacyFormat, true);
            if (legacyText && QMessageBox::question(this, tr("File Encoding"),
                    tr("Open this file using the Windows-1252 encoding?"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes) {
                content = legacyText;
                format = legacyFormat;
            }
        }
        if (!content || bytes.size() > textLimit || file.error() != QFileDevice::NoError) {
            QMessageBox::warning(this, tr("Open File"), tr("The file could not be read as supported Unicode text."));
            return;
        }
        auto blank = current();
        const bool replaceBlank = blank && tabs->count() == 1 && blank->path.isEmpty() &&
                                  blank->editor->document()->isEmpty();
        auto d = addDocument(*content, resolved, format);
        d->diskVersion = hash(bytes);
        watch(d);
        if (replaceBlank)
            removeDocument(blank);
        recent.removeAll(resolved);
        recent.prepend(resolved);
        if (recent.size() > 20) recent.removeLast();
        settings.setValue("recent", recent);
    }

    bool smokeTest()
    {
        QTemporaryDir files;
        if (!files.isValid()) return false;
        const auto documentPath = files.filePath("draft.txt");
        if (!writeFile(documentPath, "original", true) ||
            writeFile(documentPath, "replacement", true, [] { return false; }) ||
            readRegular(documentPath) != std::optional<QByteArray>(QByteArray("original")) ||
            readRegular(documentPath, 3))
            return false;
        if (!testFileWatch(documentPath)) return false;
        const auto utf8Path = files.filePath("unicode.log");
        const QString unicode = QString::fromUtf8("Aह🙂Z");
        const auto utf8 = unicode.toUtf8();
        if (!writeFile(utf8Path, utf8, true)) return false;
        QFile utf8File(utf8Path);
        if (!openRegular(utf8File)) return false;
        for (qint64 boundary = 1; boundary < utf8.size(); ++boundary) {
            const auto left = LogWindow::read(utf8File, 0, boundary, utf8.size());
            const auto right = LogWindow::read(utf8File, boundary, utf8.size(), utf8.size());
            if (!left || !right || *left + *right != unicode) return false;
        }
        const auto fifoPath = files.filePath("pipe");
        if (::mkfifo(QFile::encodeName(fifoPath).constData(), 0600) != 0 || readRegular(fifoPath))
            return false;
        const auto inserted = compareLines("heading\nthird\n", "heading\nsecond\nthird\n");
        if (!inserted || inserted->size() != 1 || inserted->first().draftStart != 1 ||
            inserted->first().draftCount != 0 || inserted->first().referenceStart != 1 ||
            inserted->first().referenceCount != 1) return false;
        const auto empty = countText({});
        const auto sample = countText("One two\nthree\n \nFour!");
        const auto combined = countText(QString::fromUtf8("e\u0301"));
        if (empty.words || empty.characters || empty.paragraphs ||
            sample.words != 4 || sample.characters != 21 || sample.paragraphs != 2 ||
            combined.words != 1 || combined.characters != 1 || combined.paragraphs != 1)
            return false;
        auto d = current();
        const QString original = QString::fromUtf8("English नमस्ते ନମସ୍କାର\nSecond line");
        d->editor->insertPlainText(original);
        d->editor->moveCursor(QTextCursor::End);
        const int originalPosition = d->editor->textCursor().position();
        for (auto button : {commandMode, textMode}) {
            button->click();
            if (current() != d || d->editor->toPlainText() != original ||
                d->editor->textCursor().position() != originalPosition ||
                !d->editor->document()->isModified() ||
                !d->editor->document()->isUndoAvailable())
                return false;
        }
        Format format{"UTF-16LE", "CRLF", false};
        Format detected;
        const auto encoded = encode(original, format);
        if (!encoded || decode(*encoded, detected) != std::optional<QString>(original) ||
            detected.encoding != format.encoding || detected.ending != format.ending)
            return false;
        auto dirty = serialize(d);
        auto clean = dirty;
        clean.insert("dirty", false);
        const QJsonObject session{{"documents", QJsonArray{clean, dirty}}, {"active", 1}};
        removeDocument(d);
        readSession(session, false);
        if (tabs->count() != 0 || archive.size() != 1 || archive.first().toObject() != dirty)
            return false;
        readSession(session, true);
        return tabs->count() == 2 && tabs->currentIndex() == 1 &&
               current()->editor->toPlainText() == dirty.value("text").toString();
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        if (sessionEnabled && flushSession()) {
            event->accept();
            return;
        }
        for (auto &item : documents) {
            if (!confirmClose(item.get())) {
                event->ignore();
                return;
            }
        }
        event->accept();
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    }

    void dropEvent(QDropEvent *event) override
    {
        for (const auto &url : event->mimeData()->urls())
            if (url.isLocalFile()) openPath(url.toLocalFile());
        event->acceptProposedAction();
    }

private:
    QAction *action(QMenu *menu, const QString &label, const QKeySequence &shortcut,
                    const std::function<void()> &callback)
    {
        auto item = menu->addAction(label);
        if (!shortcut.isEmpty()) item->setShortcut(shortcut);
        connect(item, &QAction::triggered, this, callback);
        return item;
    }

    Document *current() const
    {
        for (const auto &item : documents)
            if (item->page == tabs->currentWidget()) return item.get();
        return nullptr;
    }

    QPlainTextEdit *activeEditor() const
    {
        if (auto edit = qobject_cast<QPlainTextEdit *>(QApplication::focusWidget()))
            return edit;
        if (auto d = current()) return d->editor;
        return nullptr;
    }

    QString title(const Document *d) const
    {
        return d->path.isEmpty() ? tr("Untitled") : QFileInfo(d->path).fileName();
    }

    Document *addDocument(const QString &content = {}, const QString &path = {}, const Format &format = {})
    {
        auto entry = std::make_unique<Document>();
        auto d = entry.get();
        d->path = path;
        d->format = format;
        d->page = new QWidget;
        auto layout = new QVBoxLayout(d->page);
        layout->setContentsMargins(0, 0, 0, 0);
        d->views = new QSplitter;
        layout->addWidget(d->views);
        d->editor = new Editor;
        d->editor->correctWord = [this](QTextCursor word) {
            if (!spellEnabled || !autocorrectEnabled || !spelling.misspelled(word.selectedText())) return;
            const auto choices = spelling.suggestions(word.selectedText());
            if (choices.size() == 1) word.insertText(choices.first());
        };
        d->editor->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(d->editor, &QWidget::customContextMenuRequested, this, [this, d](const QPoint &at) {
            std::unique_ptr<QMenu> menu(d->editor->createStandardContextMenu(at));
            auto word = d->editor->cursorForPosition(at);
            word.select(QTextCursor::WordUnderCursor);
            if (spellEnabled && spelling.misspelled(word.selectedText())) {
                menu->addSeparator();
                for (const auto &suggestion : spelling.suggestions(word.selectedText()))
                    connect(menu->addAction(suggestion), &QAction::triggered, this, [word, suggestion]() mutable {
                        word.insertText(suggestion);
                    });
            }
            menu->exec(d->editor->viewport()->mapToGlobal(at));
        });
        d->views->addWidget(d->editor);
        d->editor->setPlainText(content);
        documents.push_back(std::move(entry));
        tabs->addTab(d->page, title(d));
        connect(d->editor->document(), &QTextDocument::modificationChanged, this, [this, d] { updateTab(d); });
        connect(d->editor, &QPlainTextEdit::cursorPositionChanged, this, [this] { updateStatus(); });
        connect(d->editor, &QPlainTextEdit::textChanged, this, [this, d] {
            if (d == current()) statisticsTimer->start();
        });
        tabs->setCurrentWidget(d->page);
        applyEditorSettings();
        d->editor->setFocus();
        return d;
    }

    void updateTab(Document *d)
    {
        tabs->setTabText(tabs->indexOf(d->page), title(d) +
            (d->editor->document()->isModified() ? " •" : "") + (d->diskConflict ? " ⚠" : ""));
        tabs->setTabToolTip(tabs->indexOf(d->page), d->path);
        updateStatus();
    }

    void updateStatus()
    {
        if (auto d = current()) {
            const auto cursor = d->editor->textCursor();
            position->setText(tr("Ln %1, Col %2").arg(cursor.blockNumber() + 1).arg(cursor.positionInBlock() + 1));
            details->setText(tr("%1   |   %2   |   %3   |   %4%")
                .arg(d->format.encoding, d->format.ending, wrap ? tr("Wrap On") : tr("Wrap Off"))
                .arg(100 + 10 * zoom));
        }
    }

    void updateStatistics()
    {
        statisticsTimer->stop();
        const auto d = current();
        if (!d) {
            statistics->clear();
            return;
        }
        const auto counts = countText(d->editor->toPlainText());
        statistics->setText(tr("Words %L1  |  Characters %L2  |  Paragraphs %L3")
            .arg(qlonglong(counts.words)).arg(qlonglong(counts.characters)).arg(qlonglong(counts.paragraphs)));
        underlineSpelling(d);
    }

    void refresh()
    {
        updateStatus();
        updateStatistics();
    }

    void setMode(bool enabled)
    {
        command = enabled;
        textMode->setChecked(!enabled);
        commandMode->setChecked(enabled);
        commandRow->setVisible(enabled && !focused);
        for (auto &d : documents) {
            if (d->comparison) d->comparison->setVisible(enabled && !focused);
        }
        settings.setValue("command", enabled);
        applyTheme();
        applyEditorSettings();
        if (auto d = current())
            d->editor->setFocus(Qt::OtherFocusReason);
    }

    void applyTheme()
    {
        if (theme == "System") {
            qApp->setPalette(systemPalette);
            qApp->setStyleSheet({});
            return;
        }
        const bool dark = theme == "Dark" || (theme == "Design" && command);
        const QString chrome = dark ? "#22263b" : "#eae2fa";
        const QString canvas = dark ? "#171b2b" : "#fcfaff";
        const QString foreground = dark ? "#fbf4e5" : "#211936";
        const QString border = dark ? "#525b78" : "#bdb0d6";
        QPalette palette = systemPalette;
        palette.setColor(QPalette::Window, QColor(chrome));
        palette.setColor(QPalette::Base, QColor(canvas));
        palette.setColor(QPalette::AlternateBase, QColor(chrome));
        palette.setColor(QPalette::Button, QColor(chrome));
        for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
            palette.setColor(role, QColor(foreground));
        palette.setColor(QPalette::Highlight, QColor("#ffbd62"));
        palette.setColor(QPalette::HighlightedText, QColor("#231707"));
        qApp->setPalette(palette);
        qApp->setStyleSheet(QString(
            "QPushButton,QToolButton,QLineEdit,QComboBox{border:1px solid %1;border-radius:5px;padding:5px 9px;}"
            "QPushButton:checked{background:#ffbd62;color:#231707;border-color:#e3a044;}"
            "QPushButton:hover,QToolButton:hover{border-color:#d28623;}"
            "QTabWidget::pane,QPlainTextEdit,QTextBrowser,QListWidget{border:1px solid %1;border-radius:5px;}"
            "QTabBar::tab{background:%2;padding:8px 13px;border-bottom:3px solid transparent;}"
            "QTabBar::tab:selected{background:%3;border-bottom-color:#eca031;}"
            "QSplitter::handle{background:%2;width:6px;}").arg(border, chrome, canvas));
    }

    void applyEditorSettings()
    {
        auto font = baseFont;
        const qreal size = baseFont.pointSizeF() > 0 ? baseFont.pointSizeF() : 11;
        font.setPointSizeF(qMax(6.0, size * (1 + zoom / 10.0)));
        for (auto &d : documents) {
            d->editor->setFont(font);
            d->editor->showNumbers(command && !focused);
            d->editor->setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
            if (d->comparison) d->comparison->setFont(font);
        }
        updateStatus();
    }

    void adjustZoom(int amount)
    {
        zoom = qBound(-4, zoom + amount, 10);
        applyEditorSettings();
    }

    void toggleFocus()
    {
        focused = !focused;
        auto rootLayout = centralWidget()->layout();
        rootLayout->itemAt(0)->layout()->setEnabled(!focused);
        for (int i = 0; i < rootLayout->itemAt(0)->layout()->count(); ++i) {
            if (auto widget = rootLayout->itemAt(0)->layout()->itemAt(i)->widget())
                widget->setVisible(!focused);
        }
        menuBar()->setVisible(!focused);
        tabs->tabBar()->setVisible(!focused);
        tabs->cornerWidget()->setVisible(!focused);
        statusBar()->setVisible(!focused);
        findRow->hide();
        setMode(command);
    }

    void openDialog()
    {
        for (const auto &path : QFileDialog::getOpenFileNames(this, tr("Open Text Files")))
            openPath(path);
    }

    static QByteArray hash(const QByteArray &bytes)
    {
        return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    }

    bool save(Document *d, bool saveAs = false)
    {
        QString path = d->path;
        if (saveAs || path.isEmpty()) {
            path = QFileDialog::getSaveFileName(this, tr("Save Text File"), path);
            if (path.isEmpty()) return false;
            const QString canonical = QFileInfo(path).canonicalFilePath();
            if (!canonical.isEmpty()) path = canonical;
        }
        for (auto &item : documents) {
            if (item.get() != d && item->path == path) {
                QMessageBox::warning(this, tr("Save File"), tr("This file is already open in another tab."));
                return false;
            }
        }
        bool conflict = false;
        const auto unchanged = [&] {
            if (path != d->path || d->diskVersion.isEmpty()) return true;
            const auto disk = readRegular(path);
            conflict = !disk || hash(*disk) != d->diskVersion;
            return !conflict;
        };
        const auto bytes = encode(d->editor->toPlainText(), d->format);
        if (!bytes) {
            QMessageBox::warning(this, tr("Save File"),
                tr("This encoding cannot store some characters in the document. Select a Unicode encoding and try again."));
            return false;
        }
        if (!unchanged() || !writeFile(path, *bytes, false, unchanged)) {
            if (conflict) {
                reviewDisk(d);
                return false;
            }
            QMessageBox::warning(this, tr("Save File"), tr("The file could not be saved safely. Check the destination permissions or use Save As."));
            return false;
        }
        const auto previousPath = d->path;
        d->path = path;
        d->diskVersion = hash(*bytes);
        d->diskConflict = false;
        if (previousPath != path) unwatch(previousPath);
        watch(d);
        d->editor->document()->setModified(false);
        updateTab(d);
        flushSession();
        return true;
    }

    bool confirmClose(Document *d)
    {
        if (!d->editor->document()->isModified()) return true;
        const auto choice = QMessageBox::question(this, tr("Unsaved Changes"),
            tr("Save changes to %1?").arg(title(d)), QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        return choice == QMessageBox::Discard || (choice == QMessageBox::Save && save(d));
    }

    void removeDocument(Document *d)
    {
        auto page = d->page;
        const auto previousPath = d->path;
        tabs->removeTab(tabs->indexOf(page));
        documents.erase(std::remove_if(documents.begin(), documents.end(),
            [d](const auto &item) { return item.get() == d; }), documents.end());
        unwatch(previousPath);
        delete page;
    }

    void closeTab(int index)
    {
        QWidget *page = tabs->widget(index);
        for (auto &item : documents) {
            auto d = item.get();
            if (d->page != page) continue;
            if (!confirmClose(d)) return;
            if (sessionEnabled) {
                auto candidate = archive;
                candidate.append(serialize(d));
                const bool tooLarge = QJsonDocument(candidate).toJson(QJsonDocument::Compact).size() > archiveLimit;
                if (tooLarge && d->editor->document()->isModified() &&
                    QMessageBox::question(this, tr("Recovery Storage Full"),
                        tr("This draft cannot be kept in recovery. Close it without a recovery copy?"),
                        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
                    return;
                if (!tooLarge) archive = std::move(candidate);
                // Limit ordinary closed tabs without evicting unsaved drafts.
                for (qsizetype i = 0; archive.size() > 8 && i < archive.size(); ) {
                    if (archive.at(i).toObject().value("dirty").toBool())
                        ++i;
                    else
                        archive.removeAt(i);
                }
            }
            if (!d->path.isEmpty()) closed.append(d->path);
            removeDocument(d);
            if (tabs->count() == 0) addDocument();
            flushSession();
            return;
        }
    }

    void reopen()
    {
        if (!archive.isEmpty()) {
            const auto record = archive.last().toObject();
            archive.removeLast();
            restoreRecord(record);
            flushSession();
        } else if (!closed.isEmpty()) {
            openPath(closed.takeLast());
        }
    }

    void recoverDraft()
    {
        if (archive.isEmpty()) {
            statusBar()->showMessage(tr("There are no drafts to recover."), 3000);
            return;
        }
        QStringList choices;
        for (qsizetype i = 0; i < archive.size(); ++i) {
            const auto item = archive.at(i).toObject();
            const auto path = item.value("path").toString();
            const auto firstLine = item.value("text").toString().section('\n', 0, 0).left(42);
            choices.append(tr("%1. %2 — %3").arg(i + 1)
                .arg(path.isEmpty() ? tr("Untitled") : QFileInfo(path).fileName(), firstLine));
        }
        bool ok;
        const auto choice = QInputDialog::getItem(this, tr("Recover Draft"), tr("Closed document"),
                                                choices, choices.size() - 1, false, &ok);
        if (!ok) return;
        const auto index = choices.indexOf(choice);
        restoreRecord(archive.at(index).toObject());
        archive.removeAt(index);
        flushSession();
    }

    void showFind(bool withReplace)
    {
        if (auto d = current()) {
            const auto selection = d->editor->textCursor().selectedText();
            if (!selection.isEmpty() && !selection.contains(QChar::ParagraphSeparator))
                findField->setText(selection);
        }
        replaceField->setFocusPolicy(withReplace ? Qt::StrongFocus : Qt::ClickFocus);
        findRow->show();
        findField->setFocus();
        findField->selectAll();
    }

    bool findNext(bool back = false)
    {
        auto d = current();
        if (!d || findField->text().isEmpty()) return false;
        QTextDocument::FindFlags flags;
        if (back) flags |= QTextDocument::FindBackward;
        if (matchCase->isChecked()) flags |= QTextDocument::FindCaseSensitively;
        const auto previous = d->editor->textCursor();
        if (d->editor->find(findField->text(), flags)) return true;
        d->editor->moveCursor(back ? QTextCursor::End : QTextCursor::Start);
        if (d->editor->find(findField->text(), flags)) return true;
        d->editor->setTextCursor(previous);
        statusBar()->showMessage(tr("No matches"), 2000);
        return false;
    }

    void replace(bool all)
    {
        auto d = current();
        const auto needle = findField->text();
        if (!d || needle.isEmpty()) return;
        const auto sensitivity = matchCase->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;
        if (!all) {
            auto cursor = d->editor->textCursor();
            if (cursor.selectedText().compare(needle, sensitivity) != 0) {
                findNext();
                return;
            }
            cursor.insertText(replaceField->text());
            return;
        }
        auto cursor = QTextCursor(d->editor->document());
        cursor.beginEditBlock();
        QTextDocument::FindFlags flags;
        if (matchCase->isChecked()) flags |= QTextDocument::FindCaseSensitively;
        while (true) {
            auto match = d->editor->document()->find(needle, cursor, flags);
            if (match.isNull()) break;
            match.insertText(replaceField->text());
            cursor.setPosition(match.position());
        }
        cursor.endEditBlock();
    }

    void goToLine()
    {
        auto d = current();
        if (!d) return;
        bool ok;
        const int line = QInputDialog::getInt(this, tr("Go to Line"), tr("Line number"),
            d->editor->textCursor().blockNumber() + 1, 1, d->editor->blockCount(), 1, &ok);
        if (!ok) return;
        d->editor->setTextCursor(QTextCursor(d->editor->document()->findBlockByNumber(line - 1)));
        d->editor->centerCursor();
    }

    bool testFileWatch(const QString &path)
    {
        auto waitFor = [](const std::function<bool()> &condition) {
            QEventLoop loop;
            QTimer poll;
            connect(&poll, &QTimer::timeout, &loop, [&] {
                if (condition()) loop.quit();
            });
            poll.start(10);
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            if (!condition()) loop.exec();
            return condition();
        };

        auto d = addDocument("original", path);
        d->diskVersion = hash(QByteArray("original"));
        watch(d);
        const auto backup = path + ".renamed";
        const bool passed = fileWatcher->files().contains(path) &&
            QFile::rename(path, backup) &&
            waitFor([&] { return d->diskConflict; }) &&
            writeFile(path, "original", true) &&
            waitFor([&] { return !d->diskConflict && fileWatcher->files().contains(path); }) &&
            writeFile(path, "external edit", true) &&
            waitFor([&] { return d->diskConflict; }) &&
            d->editor->toPlainText() == "original";
        removeDocument(d);
        if (!passed) qWarning("File watcher rename/recreate regression test failed");
        return passed;
    }

    void watch(Document *d)
    {
        if (d->path.isEmpty()) return;
        const QFileInfo info(d->path);
        const auto directory = info.absolutePath();
        const bool directoryWatched = fileWatcher->directories().contains(directory) ||
            fileWatcher->addPath(directory);
        const bool fileWatched = info.isFile() && (fileWatcher->files().contains(d->path) ||
            fileWatcher->addPath(d->path));
        if (!directoryWatched || (info.isFile() && !fileWatched))
            statusBar()->showMessage(tr("Could not fully monitor %1 for external changes.").arg(title(d)), 8000);
    }

    void unwatch(const QString &path)
    {
        if (path.isEmpty()) return;
        if (fileWatcher->files().contains(path)) fileWatcher->removePath(path);
        const auto directory = QFileInfo(path).absolutePath();
        for (const auto &item : documents)
            if (!item->path.isEmpty() && QFileInfo(item->path).absolutePath() == directory)
                return;
        if (fileWatcher->directories().contains(directory)) fileWatcher->removePath(directory);
    }

    void diskChanged(const QString &path)
    {
        for (auto &item : documents) {
            auto d = item.get();
            if (d->path != path) continue;
            watch(d);
            const auto bytes = readRegular(path);
            d->diskConflict = !bytes || hash(*bytes) != d->diskVersion;
            updateTab(d);
            if (!d->diskConflict) return;
            statusBar()->showMessage(tr("%1 changed on disk. Choose File → Review Disk Changes.").arg(title(d)), 8000);
            return;
        }
    }

    void reviewDisk(Document *d)
    {
        if (d->path.isEmpty()) return;
        const auto bytes = readRegular(d->path);
        Format diskFormat;
        std::optional<QString> disk;
        if (bytes) {
            disk = decode(*bytes, diskFormat);
            if (!disk && d->format.encoding == "Windows-1252") disk = decode(*bytes, diskFormat, true);
        }
        if (disk && hash(*bytes) == d->diskVersion) {
            d->diskConflict = false;
            updateTab(d);
            statusBar()->showMessage(tr("The file matches the version last loaded from disk."), 3000);
            return;
        }
        QMessageBox box(QMessageBox::Warning, tr("File Changed on Disk"),
            disk ? tr("Review the on-disk version before replacing your draft.") :
                   tr("The on-disk version is missing, unreadable, or too large to reload."), QMessageBox::NoButton, this);
        auto compare = disk ? box.addButton(tr("Compare"), QMessageBox::ActionRole) : nullptr;
        auto reload = disk ? box.addButton(tr("Reload from Disk"), QMessageBox::DestructiveRole) : nullptr;
        auto copy = box.addButton(tr("Save a Copy…"), QMessageBox::ActionRole);
        auto keep = box.addButton(tr("Keep Editing"), QMessageBox::RejectRole);
        box.setDefaultButton(keep);
        box.exec();
        auto clicked = box.clickedButton();
        if (compare && clicked == compare) {
            showComparison(d, *disk, tr("On-disk version"));
        } else if (reload && clicked == reload) {
            if (d->editor->document()->isModified() && QMessageBox::question(this, tr("Reload from Disk"),
                    tr("Replace your unsaved edits with the on-disk version?"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
            d->editor->setPlainText(*disk);
            d->format = diskFormat;
            d->diskVersion = hash(*bytes);
            d->diskConflict = false;
            d->editor->document()->setModified(false);
            watch(d);
            updateTab(d);
            flushSession();
        } else if (clicked == copy) {
            const auto path = QFileDialog::getSaveFileName(this, tr("Save a Copy"), d->path + ".copy");
            if (path.isEmpty()) return;
            const auto encoded = encode(d->editor->toPlainText(), d->format);
            if (QFileInfo(path).absoluteFilePath() == d->path || QFileInfo(path).canonicalFilePath() == d->path ||
                !encoded || !writeFile(path, *encoded))
                QMessageBox::warning(this, tr("Save a Copy"), tr("Choose a different writable location and a compatible encoding."));
            else
                statusBar()->showMessage(tr("Copy saved to %1").arg(QDir::toNativeSeparators(path)), 6000);
        }
    }

    bool flushSession()
    {
        if (!sessionEnabled) return true;
        QJsonArray saved;
        qsizetype totalText = 0;
        for (int i = 0; i < tabs->count(); ++i) {
            for (auto &item : documents) {
                auto d = item.get();
                if (d->page != tabs->widget(i)) continue;
                const auto content = d->editor->toPlainText();
                totalText += content.size();
                if (totalText > 32 * 1024 * 1024) {
                    statusBar()->showMessage(tr("Session is too large for automatic recovery."), 3500);
                    return false;
                }
                saved.append(serialize(d));
            }
        }
        const QJsonObject session{{"documents", saved}, {"active", tabs->currentIndex()}, {"archive", archive}};
        const QByteArray bytes = QJsonDocument(session).toJson(QJsonDocument::Compact);
        if (bytes.size() > recoveryLimit) {
            statusBar()->showMessage(tr("Session is too large for automatic recovery."), 3500);
            return false;
        }
        if (bytes == lastSession) return true;
        if (statePath.isEmpty() || !writeFile(statePath, bytes, true)) {
            statusBar()->showMessage(tr("Could not write draft recovery data."), 3500);
            return false;
        }
        lastSession = bytes;
        return true;
    }

    void clearRecovery()
    {
        if (QMessageBox::question(this, tr("Clear Recovery Data…"),
                tr("Delete this window’s recovery data and older snapshots? Open tabs will stay available."),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        bool removed = !QFile::exists(statePath) || QFile::remove(statePath);
        const QDir legacy(dataDirectory + "/history");
        if (!dataDirectory.isEmpty() && QFileInfo(legacy.absolutePath()).isDir() &&
            !QFileInfo(legacy.absolutePath()).isSymLink()) {
            static const QRegularExpression snapshotName("^[0-9a-f]{64}[.]json$");
            for (const auto &entry : legacy.entryList(QDir::Files | QDir::NoSymLinks)) {
                if (!snapshotName.match(entry).hasMatch()) continue;
                const auto path = legacy.filePath(entry);
                QLockFile lock(path + ".lock");
                if (!lock.tryLock(0) || !QFile::remove(path)) removed = false;
            }
        }
        archive = {};
        closed.clear();
        lastSession.clear();
        if (!removed)
            QMessageBox::warning(this, tr("Clear Recovery Data…"), tr("Some recovery files could not be removed."));
        else
            statusBar()->showMessage(tr("Recovery cleared. Open drafts can be stored again while recovery is enabled."), 6000);
    }

    void loadSession(bool restoreTabs)
    {
        if (statePath.isEmpty()) return;
        const auto bytes = readRegular(statePath, recoveryLimit);
        const auto session = bytes ? QJsonDocument::fromJson(*bytes) : QJsonDocument();
        if (session.isObject() && session.object().value("documents").isArray() &&
            session.object().value("archive").isArray()) {
            readSession(session.object(), restoreTabs);
        } else if (QFileInfo::exists(statePath)) {
            sessionEnabled = false;
            QMessageBox::warning(this, tr("Recover Draft"),
                tr("The previous recovery file could not be read. Recovery was paused to preserve it."));
        }
    }

    void readSession(const QJsonObject &session, bool restoreTabs)
    {
        archive = {};
        for (const auto &value : session.value("archive").toArray()) {
            auto record = value.toObject();
            record.remove("history");
            archive.append(record);
        }
        const auto previousTabs = session.value("documents").toArray();
        for (const auto &value : previousTabs) {
            if (restoreTabs)
                restoreRecord(value.toObject());
            else if (value.toObject().value("dirty").toBool()) {
                auto draft = value.toObject();
                draft.remove("history");
                archive.append(draft);
            }
        }
        if (restoreTabs && !previousTabs.isEmpty())
            tabs->setCurrentIndex(session.value("active").toInt(0));
    }

    QJsonObject serialize(Document *d) const
    {
        return {{"path", d->path}, {"text", d->editor->toPlainText().replace(QChar::ParagraphSeparator, '\n')},
                {"id", d->id}, {"position", d->editor->textCursor().position()},
                {"dirty", d->editor->document()->isModified()}, {"encoding", d->format.encoding},
                {"ending", d->format.ending}, {"bom", d->format.bom},
                {"hash", QString::fromLatin1(d->diskVersion.toHex())}};
    }

    void restoreRecord(const QJsonObject &item)
    {
        QString path = item.value("path").toString();
        for (auto &d : documents) {
            if (!path.isEmpty() && d->path == path) {
                path.clear();
                break;
            }
        }
        const Format format{item.value("encoding").toString("UTF-8"),
                            item.value("ending").toString("LF"), item.value("bom").toBool()};
        auto d = addDocument(item.value("text").toString(), path, format);
        d->id = item.value("id").toString();
        d->diskVersion = QByteArray::fromHex(item.value("hash").toString().toLatin1());
        const bool dirty = item.value("dirty").toBool();
        if (!dirty && !d->path.isEmpty()) {
            if (auto bytes = readRegular(d->path)) {
                Format diskFormat;
                const auto diskText = decode(*bytes, diskFormat, d->format.encoding == "Windows-1252");
                if (diskText) {
                    d->editor->setPlainText(*diskText);
                    d->format = diskFormat;
                    d->diskVersion = hash(*bytes);
                } else {
                    d->editor->document()->setModified(true);
                }
            } else {
                d->editor->document()->setModified(true);
            }
        } else {
            d->editor->document()->setModified(dirty);
        }
        auto cursor = d->editor->textCursor();
        cursor.setPosition(qBound(0, item.value("position").toInt(), d->editor->document()->characterCount() - 1));
        d->editor->setTextCursor(cursor);
        watch(d);
        updateTab(d);
    }

    void showComparison(Document *d, const QString &reference, const QString &label)
    {
        const auto draft = d->editor->toPlainText();
        const auto changes = compareLines(draft, reference);
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Compare with %1").arg(label));
        dialog.resize(1040, 630);
        QVBoxLayout layout(&dialog);
        QSplitter columns;
        layout.addWidget(&columns, 1);
        auto makeColumn = [&](const QString &title, const QString &content) {
            auto holder = new QWidget;
            auto column = new QVBoxLayout(holder);
            column->addWidget(new QLabel(title));
            auto view = new QPlainTextEdit(content);
            view->setReadOnly(true);
            view->setFont(d->editor->font());
            view->setLineWrapMode(QPlainTextEdit::NoWrap);
            column->addWidget(view);
            columns.addWidget(holder);
            return view;
        };
        auto left = makeColumn(tr("Current Draft"), draft);
        auto right = makeColumn(label, reference);
        QComboBox hunks;
        layout.addWidget(&hunks);
        if (!changes || changes->isEmpty())
            hunks.addItem(changes ? tr("No differences") : tr("Change navigation unavailable for this comparison"));
        if (changes) {
            for (const auto &change : *changes)
                hunks.addItem(tr("Draft line %1 (%2) · Reference line %3 (%4)")
                    .arg(change.draftStart + 1).arg(change.draftCount)
                    .arg(change.referenceStart + 1).arg(change.referenceCount));
        }
        auto showHunk = [&](int index) {
            if (!changes || index < 0 || index >= changes->size()) return;
            const auto change = changes->at(index);
            auto highlight = [](QPlainTextEdit *edit, int start, int count, const QColor &color) {
                auto block = edit->document()->findBlockByNumber(qMin(start, edit->blockCount() - 1));
                edit->setTextCursor(QTextCursor(block));
                edit->centerCursor();
                QList<QTextEdit::ExtraSelection> marks;
                for (int i = 0; i < count && block.isValid(); ++i, block = block.next()) {
                    QTextEdit::ExtraSelection mark;
                    mark.cursor = QTextCursor(block);
                    mark.format.setBackground(color);
                    mark.format.setProperty(QTextFormat::FullWidthSelection, true);
                    marks.append(mark);
                }
                edit->setExtraSelections(marks);
            };
            highlight(left, change.draftStart, change.draftCount, QColor(66, 150, 93, 90));
            highlight(right, change.referenceStart, change.referenceCount, QColor(230, 135, 40, 105));
        };
        connect(&hunks, &QComboBox::currentIndexChanged, &dialog, showHunk);
        showHunk(0);
        dialog.exec();
    }

    void toggleSplit()
    {
        auto d = current();
        if (!d) return;
        if (d->comparison) {
            delete d->comparison.data();
            d->comparison = nullptr;
            d->editor->setExtraSelections({});
            return;
        }
        QStringList names;
        QList<Document *> candidates;
        for (int index = 0; index < tabs->count(); ++index) {
            for (auto &item : documents) {
                if (item->page != tabs->widget(index) || d == item.get()) continue;
                names.append(tr("Tab %1 · %2").arg(index + 1).arg(title(item.get())));
                candidates.append(item.get());
            }
        }
        if (names.isEmpty()) {
            statusBar()->showMessage(tr("Open a second tab to use split editing."), 3000);
            return;
        }
        bool ok;
        const auto choice = QInputDialog::getItem(this, tr("Split View"), tr("Second document"), names, 0, false, &ok);
        if (!ok) return;
        auto target = candidates.at(names.indexOf(choice));
        auto second = new QPlainTextEdit;
        // Views share the same text document, including its undo stack.
        second->setDocument(target->editor->document());
        second->setFont(d->editor->font());
        d->comparison = second;
        d->views->addWidget(second);
        connect(target->editor, &QObject::destroyed, second, &QObject::deleteLater);
    }

    void openHtmlInBrowser()
    {
        const auto d = current();
        if (!d) return;
        const QFileInfo file(d->path);
        const auto suffix = file.suffix().toLower();
        if (!file.isFile() || (suffix != "html" && suffix != "htm") ||
            d->editor->document()->isModified()) {
            QMessageBox::information(this, tr("Open HTML in Browser"),
                tr("Save your changes to an .html or .htm file before opening it in your browser."));
            return;
        }
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(file.absoluteFilePath())))
            QMessageBox::warning(this, tr("Open HTML in Browser"), tr("Could not open the HTML file."));
    }

    void togglePreview()
    {
        auto d = current();
        if (!d) return;
        if (d->preview) {
            delete d->preview.data();
            return;
        }
        auto preview = new MarkdownPreview;
        d->preview = preview;
        preview->setOpenLinks(false);
        d->views->addWidget(preview);
        const auto render = [editor = d->editor, preview] {
            const auto features = QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) |
                QTextDocument::MarkdownNoHTML;
            preview->document()->setMarkdown(editor->toPlainText(), features);
        };
        connect(d->editor, &QPlainTextEdit::textChanged, preview, render);
        render();
    }

    void insertMarkdown(const QString &before, const QString &after, const QString &placeholder)
    {
        if (auto d = current()) {
            auto cursor = d->editor->textCursor();
            QString value = cursor.hasSelection() ? cursor.selectedText() : placeholder;
            value.replace(QChar::ParagraphSeparator, '\n');
            cursor.insertText(before + value + after);
        }
    }

    void transform(int kind)
    {
        auto d = current();
        if (!d) return;
        auto cursor = d->editor->textCursor();
        if (!cursor.hasSelection()) cursor.select(QTextCursor::Document);
        QString text = cursor.selectedText();
        text.replace(QChar::ParagraphSeparator, '\n');
        const bool trailingNewline = text.endsWith('\n');
        if (trailingNewline) text.chop(1);
        auto lines = text.split('\n');
        if (kind == 0) {
            std::stable_sort(lines.begin(), lines.end(), [](const QString &a, const QString &b) {
                return QString::localeAwareCompare(a, b) < 0;
            });
        } else if (kind == 1) {
            lines.removeDuplicates();
        } else {
            for (auto &line : lines) line.remove(QRegularExpression("[ \\t]+$"));
        }
        cursor.insertText(lines.join('\n') + (trailingNewline ? "\n" : ""));
    }

    void underlineSpelling(Document *d)
    {
        QList<QTextEdit::ExtraSelection> marks;
        if (spellEnabled && spelling.available() && d->editor->document()->characterCount() <= 30000) {
            static const QRegularExpression pattern("\\b[A-Za-z][A-Za-z']{1,60}\\b");
            auto matches = pattern.globalMatch(d->editor->toPlainText());
            while (matches.hasNext() && marks.size() < 500) {
                const auto match = matches.next();
                if (!spelling.misspelled(match.captured())) continue;
                QTextEdit::ExtraSelection mark;
                mark.cursor = QTextCursor(d->editor->document());
                mark.cursor.setPosition(int(match.capturedStart()));
                mark.cursor.setPosition(int(match.capturedEnd()), QTextCursor::KeepAnchor);
                mark.format.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
                mark.format.setUnderlineColor(QColor("#d64f60"));
                marks.append(mark);
            }
        }
        d->editor->setExtraSelections(marks);
    }

    void addCommands(QMenu *menu)
    {
        action(menu, tr("Command Palette"), QKeySequence("Ctrl+K"), [this] { paletteDialog(); });
        action(menu, tr("Markdown Preview"), {}, [this] { togglePreview(); });
        action(menu, tr("Open HTML in Browser"), {}, [this] { openHtmlInBrowser(); });
        auto formatting = menu->addMenu(tr("Markdown Formatting"));
        action(formatting, tr("Bold"), {}, [this] { insertMarkdown("**", "**", tr("bold text")); });
        action(formatting, tr("Italic"), {}, [this] { insertMarkdown("*", "*", tr("italic text")); });
        action(formatting, tr("Strikethrough"), {}, [this] { insertMarkdown("~~", "~~", tr("text")); });
        action(formatting, tr("Link"), {}, [this] { insertMarkdown("[", "](https://example.com)", tr("link text")); });
        action(formatting, tr("Heading"), {}, [this] { insertMarkdown("## ", {}, tr("Heading")); });
        action(formatting, tr("List item"), {}, [this] { insertMarkdown("- ", {}, tr("List item")); });
        action(formatting, tr("Table"), {}, [this] {
            if (auto d = current())
                d->editor->insertPlainText("\n| Column 1 | Column 2 |\n| --- | --- |\n| Value | Value |\n");
        });
        action(menu, tr("Sort Lines"), {}, [this] { transform(0); });
        action(menu, tr("Remove Duplicate Lines"), {}, [this] { transform(1); });
        action(menu, tr("Trim Trailing Spaces"), {}, [this] { transform(2); });
        action(menu, tr("File Encoding and Line Endings…"), {}, [this] { fileFormatDialog(); });
    }

    void paletteDialog()
    {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Command Palette"));
        dialog.resize(520, 370);
        QVBoxLayout layout(&dialog);
        QLineEdit filter;
        filter.setPlaceholderText(tr("Search files or commands"));
        QListWidget results;
        layout.addWidget(&filter);
        layout.addWidget(&results);
        QList<std::function<void()>> callbacks;
        QStringList labels;
        for (auto menu : menuBar()->actions()) {
            if (!menu->menu()) continue;
            for (auto item : menu->menu()->actions()) {
                if (item->isSeparator() || item->menu() || item->shortcut() == QKeySequence("Ctrl+K")) continue;
                auto label = item->text();
                labels.append(label.remove('&'));
                callbacks.append([item] { item->trigger(); });
            }
        }
        for (int i = 0; i < tabs->count(); ++i) {
            labels.append(tr("Switch to Tab %1 · %2").arg(i + 1).arg(tabs->tabText(i)));
            callbacks.append([this, i] { tabs->setCurrentIndex(i); });
        }
        for (const auto &path : recent) {
            labels.append(tr("Open · %1").arg(path));
            callbacks.append([this, path] { openPath(path); });
        }
        const auto populate = [&] {
            results.clear();
            for (int i = 0; i < labels.size(); ++i) {
                if (!labels.at(i).contains(filter.text(), Qt::CaseInsensitive)) continue;
                auto item = new QListWidgetItem(labels.at(i), &results);
                item->setData(Qt::UserRole, i);
            }
            if (results.count()) results.setCurrentRow(0);
        };
        int selected = -1;
        const auto accept = [&] {
            if (!results.currentItem()) return;
            selected = results.currentItem()->data(Qt::UserRole).toInt();
            dialog.accept();
        };
        connect(&filter, &QLineEdit::textChanged, &dialog, populate);
        connect(&filter, &QLineEdit::returnPressed, &dialog, accept);
        connect(&results, &QListWidget::itemActivated, &dialog, accept);
        QShortcut down(QKeySequence(Qt::Key_Down), &filter);
        QShortcut up(QKeySequence(Qt::Key_Up), &filter);
        down.setContext(Qt::WidgetShortcut);
        up.setContext(Qt::WidgetShortcut);
        connect(&down, &QShortcut::activated, &dialog, [&] {
            results.setCurrentRow(qMin(results.currentRow() + 1, results.count() - 1));
        });
        connect(&up, &QShortcut::activated, &dialog, [&] {
            results.setCurrentRow(qMax(results.currentRow() - 1, 0));
        });
        populate();
        filter.setFocus();
        if (dialog.exec() == QDialog::Accepted && selected >= 0) callbacks.at(selected)();
    }

    void fileFormatDialog()
    {
        auto d = current();
        if (!d) return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("File Encoding and Line Endings"));
        QFormLayout layout(&dialog);
        QComboBox encoding;
        encoding.addItems({"UTF-8", "UTF-8 BOM", "UTF-16LE", "UTF-16BE", "Windows-1252"});
        encoding.setCurrentText(d->format.encoding == "UTF-8" && d->format.bom ? "UTF-8 BOM" : d->format.encoding);
        QComboBox endings;
        endings.addItems({"LF", "CRLF", "CR"});
        endings.setCurrentText(d->format.ending);
        layout.addRow(tr("Encoding"), &encoding);
        layout.addRow(tr("Line endings"), &endings);
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        layout.addRow(&buttons);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return;
        const bool bom = encoding.currentText() == "UTF-8 BOM";
        const QString label = bom ? "UTF-8" : encoding.currentText();
        if (label != d->format.encoding || bom != d->format.bom || endings.currentText() != d->format.ending) {
            d->format = {label, endings.currentText(), bom};
            d->editor->document()->setModified(true);
        }
        updateStatus();
    }

    void printDocument()
    {
        auto d = current();
        if (!d) return;
        QPrinter printer;
        QPageSetupDialog setup(&printer, this);
        if (setup.exec() != QDialog::Accepted) return;
        QPrintDialog dialog(&printer, this);
        if (dialog.exec() == QDialog::Accepted) d->editor->print(&printer);
    }

    void showGuide()
    {
        const auto language = settings.value("language", "en").toString();
        QFile file(language == "hi" ? ":/help-hi.md" : ":/help-en.md");
        if (!file.open(QIODevice::ReadOnly)) return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("User Guide"));
        dialog.resize(840, 660);
        QVBoxLayout layout(&dialog);
        QTextBrowser guide;
        guide.setStyleSheet("QTextBrowser { background-color: #fffdf8; color: #242237; "
                            "border: 1px solid #a69fb4; border-radius: 6px; }");
        QPalette reading = guide.palette();
        reading.setColor(QPalette::Base, QColor("#fffdf8"));
        reading.setColor(QPalette::Text, QColor("#242237"));
        reading.setColor(QPalette::Link, QColor("#284f95"));
        guide.setPalette(reading);
        auto font = QApplication::font();
        font.setPointSizeF(12);
        guide.document()->setDefaultFont(font);
        guide.document()->setDocumentMargin(24);
        guide.setOpenLinks(false);
        connect(&guide, &QTextBrowser::anchorClicked, &dialog, [this](const QUrl &url) {
            if (url == jatayuWebsite()) openHelpUrl(url);
        });
        guide.document()->setMarkdown(QString::fromUtf8(file.readAll()));
        guide.moveCursor(QTextCursor::Start);
        layout.addWidget(&guide);
        QDialogButtonBox buttons(QDialogButtonBox::Close);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout.addWidget(&buttons);
        dialog.exec();
    }

    static QUrl jatayuWebsite()
    {
        return QUrl("https://jatayuos.in/");
    }

    void openHelpUrl(const QUrl &url)
    {
        if (!QDesktopServices::openUrl(url))
            QMessageBox::warning(this, tr("Help"), tr("Could not open this link: %1").arg(url.toString()));
    }

    void addHelpLink(QMenu *menu, const QString &title, const QUrl &url)
    {
        action(menu, title, {}, [this, url] { openHelpUrl(url); });
    }

    void settingsDialog()
    {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Settings"));
        QFormLayout layout(&dialog);
        QComboBox themes;
        for (const auto &item : {qMakePair(tr("Lekh design"), QString("Design")), qMakePair(tr("Light"), QString("Light")),
                                 qMakePair(tr("Dark"), QString("Dark")), qMakePair(tr("Follow System"), QString("System"))})
            themes.addItem(item.first, item.second);
        themes.setCurrentIndex(themes.findData(theme));
        layout.addRow(tr("Theme"), &themes);
        QCheckBox recover(tr("Keep local drafts"));
        recover.setChecked(sessionEnabled);
        layout.addRow(&recover);
        QCheckBox restoreTabs(tr("Reopen previous tabs on startup"));
        restoreTabs.setChecked(settings.value("restoreTabsOnStartup", false).toBool());
        restoreTabs.setEnabled(recover.isChecked());
        connect(&recover, &QCheckBox::toggled, &restoreTabs, &QCheckBox::setEnabled);
        layout.addRow(&restoreTabs);
        QCheckBox spellcheck(tr("English spellcheck"));
        spellcheck.setChecked(spellEnabled);
        spellcheck.setEnabled(spelling.available());
        layout.addRow(&spellcheck);
        QCheckBox autocorrect(tr("Autocorrect when there is one suggestion"));
        autocorrect.setChecked(autocorrectEnabled);
        autocorrect.setEnabled(spelling.available() && spellEnabled);
        connect(&spellcheck, &QCheckBox::toggled, &autocorrect, &QCheckBox::setEnabled);
        layout.addRow(&autocorrect);
        if (!spelling.available())
            layout.addRow(new QLabel(tr("Install hunspell-en-us to enable spellcheck.")));
        QComboBox language;
        language.addItem("English", "en");
        language.addItem(QString::fromUtf8("हिन्दी"), "hi");
        const auto oldLanguage = settings.value("language", "en").toString();
        language.setCurrentIndex(language.findData(oldLanguage));
        layout.addRow(tr("Language (restart required)"), &language);
        QFont selectedFont = baseFont;
        QPushButton chooseFont(tr("Editor Font…"));
        layout.addRow(&chooseFont);
        connect(&chooseFont, &QPushButton::clicked, &dialog, [&] {
            bool ok;
            const auto choice = QFontDialog::getFont(&ok, selectedFont, &dialog);
            if (ok) selectedFont = choice;
        });
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        layout.addRow(&buttons);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return;
        if (sessionEnabled && !recover.isChecked()) {
            if (QMessageBox::question(this, tr("Disable Recovery"),
                tr("Delete this window’s stored drafts? Open tabs will stay available until closed."),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
            if (QFile::exists(statePath) && !QFile::remove(statePath)) {
                QMessageBox::warning(this, tr("Disable Recovery"), tr("The stored session could not be removed."));
                return;
            }
            lastSession.clear();
            archive = {};
        }
        sessionEnabled = recover.isChecked();
        theme = themes.currentData().toString();
        baseFont = selectedFont;
        settings.setValue("session", sessionEnabled);
        settings.setValue("restoreTabsOnStartup", restoreTabs.isChecked());
        spellEnabled = spellcheck.isChecked();
        autocorrectEnabled = autocorrect.isChecked();
        settings.setValue("spellcheck", spellEnabled);
        settings.setValue("autocorrect", autocorrectEnabled);
        settings.setValue("language", language.currentData());
        for (auto &item : documents) underlineSpelling(item.get());
        if (language.currentData().toString() != oldLanguage)
            QMessageBox::information(this, tr("Language"), tr("Restart Jatayu Lekh to apply the language."));
        settings.setValue("theme", theme);
        settings.setValue("font", baseFont);
        applyTheme();
        applyEditorSettings();
        flushSession();
    }

    QSettings settings;
    Spelling spelling;
    bool spellEnabled = true, autocorrectEnabled = false;
    QPalette systemPalette;
    QFont baseFont;
    std::unique_ptr<QLockFile> sessionLock;
    std::vector<std::unique_ptr<Document>> documents;
    QString statePath, theme, dataDirectory;
    QFileSystemWatcher *fileWatcher;
    QStringList closed, recent;
    QJsonArray archive;
    QByteArray lastSession;
    QTabWidget *tabs;
    QPushButton *textMode, *commandMode;
    QWidget *commandRow, *findRow;
    QLineEdit *findField, *replaceField;
    QCheckBox *matchCase;
    QLabel *position, *details, *statistics;
    QTimer *statisticsTimer;
    QShortcut *focusShortcut;
    bool command = false, focused = false, wrap = true, sessionEnabled = true;
    int zoom = 0;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const bool smoke = app.arguments().contains("--smoke-test");
    QTemporaryDir testData;
    if (smoke) {
        if (!testData.isValid()) return 1;
        QStandardPaths::setTestModeEnabled(true);
        qputenv("XDG_DATA_HOME", testData.path().toUtf8());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, testData.path());
    }
    QCoreApplication::setOrganizationName("JatayuOS");
    QCoreApplication::setApplicationName(smoke ? "Jatayu Lekh Test" : "Jatayu Lekh");
    QCoreApplication::setApplicationVersion("0.2.6");
    QApplication::setDesktopFileName("in.jatayuos.Lekh");
    HindiTranslator hindi;
    if (QSettings().value("language", "en").toString() == "hi")
        app.installTranslator(&hindi);
    Window window;
    if (smoke) return window.smokeTest() ? 0 : 1;
    for (const auto &path : app.arguments().mid(1)) window.openPath(path);
    window.show();
    return app.exec();
}
