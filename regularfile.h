#pragma once
#include <QFile>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

inline bool openRegular(QFile &file)
{
    const auto path = QFile::encodeName(file.fileName());
    const int fd = ::open(path.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat info {};
    if (::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) return true;
    ::close(fd);
    return false;
}

