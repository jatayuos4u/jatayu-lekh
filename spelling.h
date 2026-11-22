#pragma once
#include <QtCore>

class Spelling {
public:
    Spelling() : library("hunspell-1.7", 0)
    {
        const QStringList dictionaries{"/usr/share/hunspell/en_US", "/usr/share/hunspell/en_GB"};
        for (const auto &name : dictionaries) {
            if (!QFileInfo::exists(name + ".aff") || !QFileInfo::exists(name + ".dic")) continue;
            auto create = reinterpret_cast<void *(*)(const char *, const char *)>(library.resolve("Hunspell_create"));
            destroy = reinterpret_cast<void (*)(void *)>(library.resolve("Hunspell_destroy"));
            spell = reinterpret_cast<int (*)(void *, const char *)>(library.resolve("Hunspell_spell"));
            suggest = reinterpret_cast<int (*)(void *, char ***, const char *)>(library.resolve("Hunspell_suggest"));
            freeList = reinterpret_cast<void (*)(void *, char ***, int)>(library.resolve("Hunspell_free_list"));
            if (create && destroy && spell && suggest && freeList)
                handle = create(QFile::encodeName(name + ".aff").constData(), QFile::encodeName(name + ".dic").constData());
            if (handle) break;
        }
    }

    ~Spelling() { if (handle) destroy(handle); }
    bool available() const { return handle != nullptr; }

    bool misspelled(const QString &word) const
    {
        static const QRegularExpression english("^[a-zA-Z][a-zA-Z']{1,60}$");
        return handle && english.match(word).hasMatch() &&
            !spell(handle, word.toUtf8().constData());
    }

    QStringList suggestions(const QString &word) const
    {
        if (!handle) return {};
        char **result = nullptr;
        const int count = suggest(handle, &result, word.toUtf8().constData());
        QStringList values;
        for (int i = 0; i < count && i < 8; ++i) values.append(QString::fromUtf8(result[i]));
        if (count > 0) freeList(handle, &result, count);
        values.removeDuplicates();
        return values;
    }

private:
    QLibrary library;
    void *handle = nullptr;
    void (*destroy)(void *) = nullptr;
    int (*spell)(void *, const char *) = nullptr;
    int (*suggest)(void *, char ***, const char *) = nullptr;
    void (*freeList)(void *, char ***, int) = nullptr;
};
