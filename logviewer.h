#pragma once

#include <QtWidgets>
#include "regularfile.h"
#include <QtCore>
#include <optional>

namespace LogWindow {

// Move a boundary forward only when it falls inside a valid UTF-8 sequence.
inline qint64 utf8Boundary(QFile &file, qint64 offset, qint64 size)
{
    if (offset <= 0 || offset >= size) return offset;
    const auto from = qMax(qint64(0), offset - 3);
    if (!file.seek(from)) return offset;
    const auto bytes = file.read(qMin(size - from, offset - from + 3));
    const auto boundary = offset - from;
    if (bytes.size() <= boundary) return offset;
    for (qint64 start = boundary - 1; start >= 0; --start) {
        const auto lead = static_cast<unsigned char>(bytes.at(start));
        if ((lead & 0xc0) == 0x80) continue;
        const int length = lead >= 0xc2 && lead <= 0xdf ? 2 :
            lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 1;
        if (start + length <= boundary || start + length > bytes.size()) break;
        QStringDecoder decoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
        const QString character = decoder(bytes.mid(start, length));
        if (!decoder.hasError() && !character.isEmpty()) return from + start + length;
        break;
    }
    return offset;
}

inline std::optional<QString> read(QFile &file, qint64 start, qint64 end, qint64 size)
{
    const auto first = utf8Boundary(file, start, size);
    const auto last = utf8Boundary(file, end, size);
    if (!file.seek(first)) return std::nullopt;
    const auto bytes = file.read(last - first);
    if (file.error() != QFileDevice::NoError) return std::nullopt;
    return QString::fromUtf8(bytes);
}

}

class LogViewer : public QDialog {
public:
    LogViewer(const QString &path, QWidget *parent) : QDialog(parent), path(path)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(tr("Large File Viewer — %1").arg(QFileInfo(path).fileName()));
        resize(950, 650);
        auto layout = new QVBoxLayout(this);
        auto controls = new QHBoxLayout;
        layout->addLayout(controls);
        auto previous = new QPushButton(tr("Previous Chunk"));
        auto next = new QPushButton(tr("Next Chunk"));
        auto follow = new QCheckBox(tr("Follow End"));
        search = new QLineEdit;
        search->setPlaceholderText(tr("Find in file (UTF-8)"));
        auto find = new QPushButton(tr("Find Next"));
        for (QWidget *widget : QList<QWidget *>{previous, next, follow, search, find})
            controls->addWidget(widget);
        editor = new QPlainTextEdit;
        editor->setReadOnly(true);
        editor->setLineWrapMode(QPlainTextEdit::NoWrap);
        editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        layout->addWidget(editor);
        status = new QLabel;
        layout->addWidget(status);
        auto timer = new QTimer(this);
        timer->setInterval(1000);
        connect(previous, &QPushButton::clicked, this, [=] {
            follow->setChecked(false);
            load(qMax(qint64(0), offset - chunk), false);
        });
        connect(next, &QPushButton::clicked, this, [=] {
            follow->setChecked(false);
            if (offset + chunk < size) load(offset + chunk, false);
        });
        auto followEnd = [this] {
            QFileInfo info(this->path);
            if (info.size() != size || info.lastModified() != modified)
                load(qMax(qint64(0), info.size() - chunk), true);
        };
        connect(timer, &QTimer::timeout, this, followEnd);
        connect(follow, &QCheckBox::toggled, this, [=](bool enabled) {
            if (enabled) {
                load(qMax(qint64(0), QFileInfo(this->path).size() - chunk), true);
                timer->start();
            } else {
                timer->stop();
            }
        });
        connect(find, &QPushButton::clicked, this, [=] {
            follow->setChecked(false);
            findNext();
        });
        connect(search, &QLineEdit::returnPressed, find, &QPushButton::click);
        connect(search, &QLineEdit::textChanged, this, [this] { nextSearch = offset; });
        load(0, false);
    }

private:
    void load(qint64 start, bool end)
    {
        QFile file(path);
        if (!openRegular(file)) {
            status->setText(tr("The file is unavailable."));
            return;
        }
        load(file, start, end);
    }

    bool load(QFile &file, qint64 start, bool end)
    {
        const auto currentSize = file.size();
        start = qBound(qint64(0), start, currentSize);
        const auto stop = start + qMin(chunk, currentSize - start);
        const auto content = LogWindow::read(file, start, stop, currentSize);
        if (!content) {
            status->setText(tr("The file is unavailable."));
            return false;
        }
        size = currentSize;
        modified = QFileInfo(file).lastModified();
        offset = start;
        editor->setPlainText(*content);
        editor->moveCursor(end ? QTextCursor::End : QTextCursor::Start);
        nextSearch = offset;
        status->setText(tr("Read-only · Bytes %L1–%L2 of %L3")
            .arg(LogWindow::utf8Boundary(file, start, size))
            .arg(LogWindow::utf8Boundary(file, stop, size)).arg(size));
        return true;
    }

    void findNext()
    {
        const auto needle = search->text().toUtf8();
        if (needle.isEmpty() || needle.size() > 4096) return;
        QFile file(path);
        if (!openRegular(file)) return;
        const qint64 total = file.size();
        QProgressDialog progress(tr("Searching file…"), tr("Cancel"), 0, 100, this);
        progress.setMinimumDuration(200);
        progress.setWindowModality(Qt::WindowModal);
        for (qint64 at = qMin(nextSearch, total); at < total; at += chunk) {
            progress.setValue(int(100.0 * at / total));
            if (progress.wasCanceled() || !file.seek(at)) return;
            const auto bytes = file.read(chunk + needle.size() - 1);
            if (bytes.isEmpty()) break;
            const auto hit = bytes.indexOf(needle);
            if (hit < 0) continue;
            if (!load(file, at + hit, false)) return;
            auto cursor = editor->textCursor();
            cursor.setPosition(qMin(int(search->text().size()), editor->document()->characterCount() - 1),
                               QTextCursor::KeepAnchor);
            editor->setTextCursor(cursor);
            nextSearch = at + hit + needle.size();
            return;
        }
        nextSearch = 0;
        status->setText(tr("No further matches. Search again to start at the beginning."));
    }

    static constexpr qint64 chunk = 256 * 1024;
    QString path;
    QPlainTextEdit *editor;
    QLabel *status;
    QLineEdit *search;
    QDateTime modified;
    qint64 offset = 0, size = -1, nextSearch = 0;
};
