#pragma once

#include <QtWidgets>
#include "regularfile.h"
#include <QtCore>

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
        if (!openRegular(file) || !file.seek(start)) {
            status->setText(tr("The file is unavailable."));
            return;
        }
        size = file.size();
        modified = QFileInfo(file).lastModified();
        offset = start;
        editor->setPlainText(QString::fromUtf8(file.read(chunk)));
        editor->moveCursor(end ? QTextCursor::End : QTextCursor::Start);
        nextSearch = offset;
        status->setText(tr("Read-only · Bytes %L1–%L2 of %L3").arg(offset).arg(qMin(offset + chunk, size)).arg(size));
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
            load(at + hit, false);
            auto cursor = editor->textCursor();
            cursor.setPosition(search->text().size(), QTextCursor::KeepAnchor);
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
