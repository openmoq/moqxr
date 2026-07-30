#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openmoq::publisher {

struct ByteSpan {
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct Mp4Box {
    std::string type;
    ByteSpan span;
    ByteSpan payload;
    std::vector<Mp4Box> children;
};

// Offsets from the start of a sample entry box (including its 8-byte header)
// to its first child box, per ISO/IEC 14496-12.
//
// A sample entry payload begins with the 8-byte SampleEntry base
// (reserved[6] + data_reference_index). VisualSampleEntry adds 70 bytes on top
// of that base, for a 78-byte payload; AudioSampleEntry adds 20, for 28. The
// visual figure is why width and height are read at payload offsets 24 and 26.
//
// These are named once because the visual value was previously written as
// `8 + 70` at five separate call sites -- omitting the SampleEntry base -- so
// every child lookup landed 8 bytes early, inside compressorname's padding.
inline constexpr std::size_t kVisualSampleEntryChildOffset = 8 + 78;
inline constexpr std::size_t kAudioSampleEntryChildOffset = 8 + 28;

// Common Encryption parameters for one track, from sinf/schm/schi/tenc.
struct CencTrackProtection {
    std::string original_format;   // frma, e.g. "avc1" -- the pre-encryption codec
    std::string scheme;            // schm, "cenc" or "cbcs" (CMSF 4.1.1.3)
    std::string default_kid;       // tenc default_KID as a UUID string
    std::uint8_t per_sample_iv_size = 0;
    bool is_protected = false;
};

// One DRM system's initialisation data, from a pssh box.
struct CencSystem {
    std::string system_id;         // UUID string
    std::string pssh_base64;       // the whole pssh box, Base64
};

struct TrackDescription {
    std::uint32_t track_id = 0;
    std::string handler_type;
    std::string codec;
    std::string sample_entry_type;
    std::string track_name;
    std::string packaging = "cmaf";
    std::string event_type;
    std::string mime_type;
    std::vector<std::string> depends;
    std::uint32_t timescale = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channel_count = 0;
    std::uint32_t sample_rate = 0;
    double frame_rate = 0.0;
    std::uint64_t max_bitrate = 0;   // bits per second, 0 when unknown
    std::uint64_t avg_bitrate = 0;   // bits per second, 0 when unknown
    std::uint64_t duration_ms = 0;   // track duration, 0 when unknown
    std::string language{};          // ISO-639-2/T, empty when unknown or "und"
    // Present when the sample entry is encv or enca and its sinf parsed.
    std::optional<CencTrackProtection> protection;
    std::vector<std::uint8_t> codec_private;  // avcC, hvcC, or esds box bytes (including box header)
};

struct ParsedMp4 {
    std::vector<std::uint8_t> bytes;
    std::vector<Mp4Box> top_level_boxes;
    std::vector<TrackDescription> tracks;
};

ParsedMp4 parse_mp4_file(const std::string& path);
ParsedMp4 parse_mp4_stream(std::istream& input, std::string_view source_name);
std::vector<Mp4Box> parse_mp4_boxes(std::span<const std::uint8_t> bytes);
std::vector<TrackDescription> extract_tracks(const std::vector<Mp4Box>& top_level_boxes,
                                             std::span<const std::uint8_t> bytes);

const Mp4Box* find_first_box(const std::vector<Mp4Box>& boxes, std::string_view type);
std::vector<const Mp4Box*> find_boxes(const std::vector<Mp4Box>& boxes, std::string_view type);
const Mp4Box* find_child_box(const Mp4Box& box, std::string_view type);
std::span<const std::uint8_t> slice_bytes(std::span<const std::uint8_t> bytes, const ByteSpan& span);

// Incremental MP4 box reader for streaming input (e.g. piped ffmpeg).
// Buffers raw bytes and yields complete top-level boxes one at a time.
struct StreamingBoxResult {
    std::string type;
    std::vector<std::uint8_t> bytes;
};

class StreamingMp4Reader {
public:
    // Append raw data to internal buffer.
    void append(const std::uint8_t* data, std::size_t len);

    // Read up to chunk_size bytes from input and append.
    // Returns number of bytes read; 0 means EOF.
    std::size_t read_from(std::istream& input, std::size_t chunk_size = 16384);

    // Try to extract the next complete top-level box from the buffer.
    // Returns std::nullopt if not enough data is available yet.
    std::optional<StreamingBoxResult> next_box();

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t consumed_ = 0;

    void compact();
};

}  // namespace openmoq::publisher
