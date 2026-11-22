#pragma once

#include <QtCore>
#include <optional>

struct Change {
    int draftStart, draftCount, referenceStart, referenceCount;
};

inline std::optional<QList<Change>> compareLines(const QString &draft, const QString &reference)
{
    if (draft.size() > 512 * 1024 || reference.size() > 512 * 1024) return std::nullopt;
    QTemporaryFile oldFile, newFile;
    if (!oldFile.open() || !newFile.open()) return std::nullopt;
    const auto oldBytes = draft.toUtf8(), newBytes = reference.toUtf8();
    if (oldFile.write(oldBytes) != oldBytes.size() || newFile.write(newBytes) != newBytes.size() ||
        !oldFile.flush() || !newFile.flush()) return std::nullopt;
    const QString format = "%df %dn %dF %dN\n";
    QProcess process;
    process.start("/usr/bin/diff", {"--text", "--unchanged-group-format=",
        "--changed-group-format=" + format, "--old-group-format=" + format,
        "--new-group-format=" + format, "--", oldFile.fileName(), newFile.fileName()});
    if (!process.waitForFinished(3000)) {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(1000);
        }
        return std::nullopt;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() > 1) return std::nullopt;
    QList<Change> results;
    for (const auto &row : process.readAllStandardOutput().split('\n')) {
        if (row.isEmpty()) continue;
        const auto parts = row.split(' ');
        if (parts.size() != 4) return std::nullopt;
        results.append({parts[0].toInt() - 1, parts[1].toInt(),
                        parts[2].toInt() - 1, parts[3].toInt()});
    }
    return results;
}
