#pragma once

// The directory tree of a disc image or an archive, read once when the source is opened: entries
// found by path, names matched regardless of ASCII case. One thread builds it; after that it does not
// change, so any thread may read it. Nothing here throws except for an allocation failure.

#include "vfs/vfs.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vfs
{
    class Tree
    {
    public:
        struct Node
        {
            std::string name;
            bool directory = false;
            uint64_t offset = 0;            // where a file's data starts, in the source's own terms
            uint64_t size = 0;
            std::vector<uint32_t> children; // a folder's, in the order they were added
        };

        static constexpr uint32_t kRoot = 0;
        static constexpr uint32_t kNone = UINT32_MAX;

        // A tree with only the root folder.
        Tree();

        // The time reported for every entry: a disc and an archive have no time per file.
        void SetTime(uint64_t time) { time_ = time; }

        // Adds an entry called name to the folder parent and returns its index. kNone, logged, when
        // parent already holds a name equal to it but for case: the first one stays.
        uint32_t Add(uint32_t parent, std::string name, bool directory, uint64_t offset, uint64_t size);

        // The node at path ("" is the root), or null when nothing is there.
        const Node* At(std::string_view path) const;

        bool Find(std::string_view path, Entry& entry) const;
        bool List(std::string_view path, std::vector<Entry>& entries) const;

        size_t Files() const { return files_; }

    private:
        Entry EntryOf(const Node& node) const;

        std::vector<Node> nodes_;                          // the root first
        std::vector<std::string> keys_;                    // each node's path as a key of byPath_
        std::unordered_map<std::string, uint32_t> byPath_; // key -> index in nodes_
        uint64_t time_ = 0;
        size_t files_ = 0;
    };
}
