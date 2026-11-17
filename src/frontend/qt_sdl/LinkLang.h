/*
    melonDS Link - languages of everything that belongs to 3DS Link.

    English is built in and the default; German is built in too. Both are
    also written to melonDS-Link/Language/<code>.txt ("key=value" lines,
    UTF-8), where they can be edited. Further languages: another file with
    the same keys (missing keys fall back to English).
*/

#ifndef MELONDS_LINKLANG_H
#define MELONDS_LINKLANG_H

#include <string>
#include <utility>
#include <vector>

#include <QString>

namespace Link
{

// the text for `key` in the current language (%1, %2... for QString::arg)
QString Tr(const char* key);
// in a given language (falls back to English)
QString TrIn(const std::string& code, const char* key);
std::string TrS(const char* key);

// load the built-in texts, write/read the Language folder, pick `code`
void InitLanguages(const std::string& code);
void SetLanguage(const std::string& code);
std::string Language();

// available languages: code + name (in that language)
std::vector<std::pair<std::string, QString>> Languages();

}

#endif
