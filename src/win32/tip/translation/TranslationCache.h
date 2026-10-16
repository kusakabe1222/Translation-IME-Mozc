#pragma once

#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "HttpTranslator.h"

// Simple LRU cache of translation results.
// Not thread-safe: only the IME (UI) thread may use it.
class CTranslationCache
{
public:
    CTranslationCache(size_t maxEntries = 100) : _maxEntries(maxEntries) {}

    // The key identifies the engine, source and target languages as well as the text.
    static std::wstring MakeKey(const TranslationConfig& config, const std::wstring& sourceLanguage, const std::wstring& sourceText)
    {
        std::wstring key = config.EngineName();
        key += L':';
        key += sourceLanguage.empty() ? L"auto" : sourceLanguage;
        for (const std::wstring& language : config.targetLanguages)
        {
            key += L',';
            key += language;
        }
        key += L'\n';
        key += sourceText;
        return key;
    }

    bool Lookup(const std::wstring& key, std::vector<TranslationItem>& items)
    {
        auto it = _cache.find(key);
        if (it == _cache.end())
        {
            return false;
        }
        _lruList.splice(_lruList.begin(), _lruList, it->second.lruIterator);
        items = it->second.items;
        return true;
    }

    void Store(const std::wstring& key, const std::vector<TranslationItem>& items)
    {
        auto it = _cache.find(key);
        if (it != _cache.end())
        {
            it->second.items = items;
            _lruList.splice(_lruList.begin(), _lruList, it->second.lruIterator);
            return;
        }

        if (_cache.size() >= _maxEntries && !_lruList.empty())
        {
            _cache.erase(_lruList.back());
            _lruList.pop_back();
        }

        _lruList.push_front(key);
        CacheEntry entry;
        entry.items = items;
        entry.lruIterator = _lruList.begin();
        _cache[key] = entry;
    }

    void Clear()
    {
        _cache.clear();
        _lruList.clear();
    }

private:
    struct CacheEntry
    {
        std::vector<TranslationItem> items;
        std::list<std::wstring>::iterator lruIterator;
    };

    size_t _maxEntries;
    std::unordered_map<std::wstring, CacheEntry> _cache;
    std::list<std::wstring> _lruList;
};
