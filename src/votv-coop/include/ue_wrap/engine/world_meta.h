// ue_wrap/engine/world_meta.h -- the 0.10.0 world folder's files read off disk, for the save picker.
//
// Engine-wrapper layer (principle 7): understands the format the game's CompressedSaveLoadLib
// writes, and nothing about co-op. A 0.10.0 world is a folder under SaveGames: main.sav/.meta/.png,
// autosave.sav/.meta/.png, persistent.sav. Every one of those files is UE's chunked compression
// (FCompressedChunkInfo, tag 0x9E2A83C1, zlib) over a payload that is an int32 length, the
// SaveGame's class path as an FString and a tagged property stream -- no GVAS header. The .meta
// file holds a save_metadata_C: version, last-saved date, game mode, rules, slot identity, level.
// Pure file I/O, callable from any thread: the picker reads every world's metadata on a worker.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::world_meta {

// True when `data` starts with the compressed-chunk tag: the 0.10.0 world files, as against a GVAS
// slot written by UGameplayStatics::SaveGameToSlot.
bool IsCompressed(const uint8_t* data, size_t size);

// Inflate a whole compressed file (every chunk group in turn) into `out`. False, with `out`
// cleared, on a bad tag, a size past `maxBytes`, or a chunk that does not inflate to its stated size.
bool Decompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                size_t maxBytes = size_t{64} << 20);

// The size a save file stands for: its inflated size when it is compressed (the sum of its chunk
// groups' stated sizes, read from the headers alone), else its size on disk. False when the file
// cannot be read or a compressed header is malformed. Any thread.
bool LogicalSize(const std::wstring& path, uint64_t& out);

struct WorldMeta {
    bool         parsed = false;          // payload inflated and the class path read
    bool         isMetadataClass = false; // the class path names save_metadata_C
    bool         hasVersion = false;      std::wstring version;
    bool         hasLastSaved = false;    int64_t lastSavedTicks = 0;  // FDateTime ticks (100 ns)
    bool         hasGameMode = false;     int gameMode = 0;            // enum_gamemode ordinal
    bool         hasSavedTimeZ = false;   int32_t savedTimeZ = 0;      // savedtime.Z, the day - 1
    std::wstring saveSlot;      // the world's folder name as the game stamped it
    std::wstring metadataName;  // "main" or "autosave"
    std::wstring area;          // non-empty when the entry was saved inside an area level
    std::wstring currentLevel;
    std::wstring starterMap;
};

// Parse an inflated metadata payload. A stream that goes malformed after the class path stops the
// walk and still returns true with what was read; false only when the class path was not reached.
bool ParseMetaPayload(const uint8_t* data, size_t size, WorldMeta& out);

// Read and parse one .meta file. Any thread.
bool ReadWorldMeta(const std::wstring& metaPath, WorldMeta& out);

}  // namespace ue_wrap::world_meta
