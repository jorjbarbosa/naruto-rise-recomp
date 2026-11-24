// The directory tree of a disc image or an archive, found by path (tree.h).

#include "vfs/tree.h"

#include "ascii.h"
#include "log.h"

#include <algorithm>
#include <utility>

namespace vfs
{
    namespace
    {
        // A path as a key of the node map: lower-case names joined by '\', no empty names.
        std::string Key(std::string_view path)
        {
            std::string key;
            size_t begin = 0;
            while (begin < path.size())
            {
                size_t end = std::min(path.find_first_of("\\/", begin), path.size());
                if (end > begin)
                {
                    if (!key.empty())
                        key += '\\';
                    for (char c : path.substr(begin, end - begin))
                        key += ascii::ToLower(c);
                }
                begin = end + 1;
            }
            return key;
        }
    }

    Tree::Tree()
    {
        nodes_.push_back(Node{ "", true });
        keys_.emplace_back();
        byPath_.emplace("", kRoot);
    }

    uint32_t Tree::Add(uint32_t parent, std::string name, bool directory, uint64_t offset, uint64_t size)
    {
        std::string key = keys_[parent].empty() ? Key(name) : keys_[parent] + '\\' + Key(name);
        uint32_t index = uint32_t(nodes_.size());
        if (!byPath_.emplace(key, index).second)
        {
            // An image or archive made on a file system that tells case apart may hold both.
            Log("game: %s left out: another entry has the same name but for case", key.c_str());
            return kNone;
        }
        nodes_.push_back(Node{ std::move(name), directory, offset, directory ? 0 : size });
        keys_.push_back(std::move(key));
        nodes_[parent].children.push_back(index);
        if (!directory)
            files_++;
        return index;
    }

    const Tree::Node* Tree::At(std::string_view path) const
    {
        auto it = byPath_.find(Key(path));
        return it != byPath_.end() ? &nodes_[it->second] : nullptr;
    }

    bool Tree::Find(std::string_view path, Entry& entry) const
    {
        const Node* node = At(path);
        if (node == nullptr)
            return false;
        entry = EntryOf(*node);
        return true;
    }

    bool Tree::List(std::string_view path, std::vector<Entry>& entries) const
    {
        entries.clear();
        const Node* node = At(path);
        if (node == nullptr || !node->directory)
            return false;
        for (uint32_t child : node->children)
            entries.push_back(EntryOf(nodes_[child]));
        return true;
    }

    Entry Tree::EntryOf(const Node& node) const
    {
        return Entry{ node.name, node.directory, node.size, time_ };
    }
}
