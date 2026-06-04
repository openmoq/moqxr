#include "openmoq/publisher/live_srt_ingest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#if defined(OPENMOQ_HAS_SRT)
#include <arpa/inet.h>
#include <netdb.h>
#include <srt/srt.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace openmoq::publisher {
namespace {

std::uint16_t read_be16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) | bytes[offset + 1]);
}

std::uint32_t read_be24(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 2]);
}

std::uint32_t read_be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

void append_be16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_be64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8U)) & 0xFFU));
    }
}

void append_ascii(std::vector<std::uint8_t>& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

std::vector<std::uint8_t> make_box(std::string_view type, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> out;
    out.reserve(8 + payload.size());
    append_be32(out, static_cast<std::uint32_t>(8 + payload.size()));
    append_ascii(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> make_full_box(std::string_view type,
                                        std::uint8_t version,
                                        std::uint32_t flags,
                                        std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> body;
    body.reserve(4 + payload.size());
    body.push_back(version);
    body.push_back(static_cast<std::uint8_t>((flags >> 16U) & 0xFFU));
    body.push_back(static_cast<std::uint8_t>((flags >> 8U) & 0xFFU));
    body.push_back(static_cast<std::uint8_t>(flags & 0xFFU));
    body.insert(body.end(), payload.begin(), payload.end());
    return make_box(type, body);
}

std::vector<std::uint8_t> build_ftyp_box() {
    std::vector<std::uint8_t> payload;
    append_ascii(payload, "isom");
    append_be32(payload, 0x00000200);
    append_ascii(payload, "isom");
    append_ascii(payload, "iso6");
    append_ascii(payload, "mp41");
    return make_box("ftyp", payload);
}

std::vector<std::uint8_t> build_mvhd_box() {
    std::vector<std::uint8_t> payload;
    append_be32(payload, 0);
    append_be32(payload, 0);
    append_be32(payload, 1000);
    append_be32(payload, 0);
    append_be32(payload, 0x00010000);
    append_be16(payload, 0x0100);
    append_be16(payload, 0);
    payload.insert(payload.end(), 8, 0);
    append_be32(payload, 0x00010000);
    append_be32(payload, 0);
    append_be32(payload, 0);
    append_be32(payload, 0);
    append_be32(payload, 0x00010000);
    append_be32(payload, 0);
    append_be32(payload, 0);
    append_be32(payload, 0);
    append_be32(payload, 0x40000000);
    payload.insert(payload.end(), 24, 0);
    append_be32(payload, 0xFFFFFFFF);
    return make_full_box("mvhd", 0, 0, payload);
}

std::vector<std::uint8_t> build_tkhd_box(const TrackDescription& track) {
    std::vector<std::uint8_t> payload;
    append_be32(payload, 0);  // creation_time
    append_be32(payload, 0);  // modification_time
    append_be32(payload, track.track_id);  // track_ID
    append_be32(payload, 0);  // reserved
    append_be32(payload, 0);  // duration
    append_be32(payload, 0);  // reserved[0]
    append_be32(payload, 0);  // reserved[1]
    append_be16(payload, 0);  // layer
    append_be16(payload, 0);  // alternate_group
    append_be16(payload, track.handler_type == "soun" ? 0x0100 : 0);  // volume
    append_be16(payload, 0);  // reserved
    // matrix (identity)
    append_be32(payload, 0x00010000);
    append_be32(payload, 0);
    append_be32(payload, 0);
    append_be32(payload, 0);
    append_be32(payload, 0x00010000);
    append_be32(payload, 0);
    append_be32(payload, 0);
    append_be32(payload, 0);
    append_be32(payload, 0x40000000);
    // width and height (16.16 fixed point)
    append_be32(payload, track.width << 16U);
    append_be32(payload, track.height << 16U);
    return make_full_box("tkhd", 0, 0x000007, payload);
}

std::vector<std::uint8_t> build_mdhd_box(const TrackDescription& track) {
    std::vector<std::uint8_t> payload;
    append_be32(payload, 0);
    append_be32(payload, 0);
    append_be32(payload, track.timescale == 0 ? 90000 : track.timescale);
    append_be32(payload, 0);
    append_be16(payload, 0x55C4);
    append_be16(payload, 0);
    return make_full_box("mdhd", 0, 0, payload);
}

std::vector<std::uint8_t> build_hdlr_box(const TrackDescription& track) {
    std::vector<std::uint8_t> payload;
    append_be32(payload, 0);
    append_ascii(payload, track.handler_type == "soun" ? "soun" : "vide");
    payload.insert(payload.end(), 12, 0);
    const std::string name = track.track_name;
    payload.insert(payload.end(), name.begin(), name.end());
    payload.push_back(0);
    return make_full_box("hdlr", 0, 0, payload);
}

std::vector<std::uint8_t> build_stsd_box(const TrackDescription& track) {
    std::vector<std::uint8_t> sample_entry;
    sample_entry.insert(sample_entry.end(), 6, 0);
    append_be16(sample_entry, 1);
    if (track.handler_type == "soun") {
        sample_entry.insert(sample_entry.end(), 8, 0);
        append_be16(sample_entry, static_cast<std::uint16_t>(track.channel_count == 0 ? 2 : track.channel_count));
        append_be16(sample_entry, 16);
        append_be16(sample_entry, 0);
        append_be16(sample_entry, 0);
        append_be32(sample_entry, (track.sample_rate == 0 ? 48000 : track.sample_rate) << 16U);
    } else {
        sample_entry.insert(sample_entry.end(), 16, 0);
        append_be16(sample_entry, static_cast<std::uint16_t>(track.width == 0 ? 1920 : track.width));
        append_be16(sample_entry, static_cast<std::uint16_t>(track.height == 0 ? 1080 : track.height));
        append_be32(sample_entry, 0x00480000);
        append_be32(sample_entry, 0x00480000);
        append_be32(sample_entry, 0);
        append_be16(sample_entry, 1);
        sample_entry.insert(sample_entry.end(), 32, 0);
        append_be16(sample_entry, 24);
        append_be16(sample_entry, 0xFFFF);
    }

    // Append codec-specific box (avcC, hvcC, esds) inside the sample entry if available
    if (!track.codec_private.empty()) {
        sample_entry.insert(sample_entry.end(), track.codec_private.begin(), track.codec_private.end());
    }

    std::vector<std::uint8_t> sample_entry_box;
    append_be32(sample_entry_box, static_cast<std::uint32_t>(8 + sample_entry.size()));
    append_ascii(sample_entry_box, track.sample_entry_type.empty() ? (track.handler_type == "soun" ? "mp4a" : "avc1")
                                                                   : track.sample_entry_type.substr(0, 4));
    sample_entry_box.insert(sample_entry_box.end(), sample_entry.begin(), sample_entry.end());

    std::vector<std::uint8_t> payload;
    append_be32(payload, 1);
    payload.insert(payload.end(), sample_entry_box.begin(), sample_entry_box.end());
    return make_full_box("stsd", 0, 0, payload);
}

std::vector<std::uint8_t> build_empty_time_table(std::string_view type) {
    std::vector<std::uint8_t> payload;
    append_be32(payload, 0);
    return make_full_box(type, 0, 0, payload);
}

std::vector<std::uint8_t> build_stsz_box() {
    std::vector<std::uint8_t> payload;
    append_be32(payload, 0);
    append_be32(payload, 0);
    return make_full_box("stsz", 0, 0, payload);
}

std::vector<std::uint8_t> build_stco_box() {
    std::vector<std::uint8_t> payload;
    append_be32(payload, 0);
    return make_full_box("stco", 0, 0, payload);
}

std::vector<std::uint8_t> build_dinf_box() {
    std::vector<std::uint8_t> url_payload;
    auto url = make_full_box("url ", 0, 1, url_payload);
    std::vector<std::uint8_t> dref_payload;
    append_be32(dref_payload, 1);
    dref_payload.insert(dref_payload.end(), url.begin(), url.end());
    auto dref = make_full_box("dref", 0, 0, dref_payload);
    return make_box("dinf", dref);
}

std::vector<std::uint8_t> build_minf_box(const TrackDescription& track) {
    std::vector<std::uint8_t> vmhd_payload;
    append_be16(vmhd_payload, 0);
    append_be16(vmhd_payload, 0);
    append_be16(vmhd_payload, 0);
    append_be16(vmhd_payload, 0);

    std::vector<std::uint8_t> smhd_payload;
    append_be16(smhd_payload, 0);
    append_be16(smhd_payload, 0);

    std::vector<std::uint8_t> stbl_payload;
    auto stsd = build_stsd_box(track);
    auto stts = build_empty_time_table("stts");
    auto stsc = build_empty_time_table("stsc");
    auto stsz = build_stsz_box();
    auto stco = build_stco_box();
    stbl_payload.insert(stbl_payload.end(), stsd.begin(), stsd.end());
    stbl_payload.insert(stbl_payload.end(), stts.begin(), stts.end());
    stbl_payload.insert(stbl_payload.end(), stsc.begin(), stsc.end());
    stbl_payload.insert(stbl_payload.end(), stsz.begin(), stsz.end());
    stbl_payload.insert(stbl_payload.end(), stco.begin(), stco.end());
    auto stbl = make_box("stbl", stbl_payload);

    auto dinf = build_dinf_box();
    std::vector<std::uint8_t> minf_payload;
    if (track.handler_type == "soun") {
        auto smhd = make_full_box("smhd", 0, 0, smhd_payload);
        minf_payload.insert(minf_payload.end(), smhd.begin(), smhd.end());
    } else {
        auto vmhd = make_full_box("vmhd", 0, 1, vmhd_payload);
        minf_payload.insert(minf_payload.end(), vmhd.begin(), vmhd.end());
    }
    minf_payload.insert(minf_payload.end(), dinf.begin(), dinf.end());
    minf_payload.insert(minf_payload.end(), stbl.begin(), stbl.end());
    return make_box("minf", minf_payload);
}

std::vector<std::uint8_t> build_trak_box(const TrackDescription& track) {
    auto tkhd = build_tkhd_box(track);
    auto mdhd = build_mdhd_box(track);
    auto hdlr = build_hdlr_box(track);
    auto minf = build_minf_box(track);

    std::vector<std::uint8_t> mdia_payload;
    mdia_payload.insert(mdia_payload.end(), mdhd.begin(), mdhd.end());
    mdia_payload.insert(mdia_payload.end(), hdlr.begin(), hdlr.end());
    mdia_payload.insert(mdia_payload.end(), minf.begin(), minf.end());
    auto mdia = make_box("mdia", mdia_payload);

    std::vector<std::uint8_t> trak_payload;
    trak_payload.insert(trak_payload.end(), tkhd.begin(), tkhd.end());
    trak_payload.insert(trak_payload.end(), mdia.begin(), mdia.end());
    return make_box("trak", trak_payload);
}

std::vector<std::uint8_t> build_mvex_box(const std::vector<TrackDescription>& tracks) {
    std::vector<std::uint8_t> payload;
    for (const auto& track : tracks) {
        std::vector<std::uint8_t> trex_payload;
        append_be32(trex_payload, track.track_id);
        append_be32(trex_payload, 1);
        append_be32(trex_payload, 0);
        append_be32(trex_payload, 0);
        append_be32(trex_payload, 0);
        auto trex = make_full_box("trex", 0, 0, trex_payload);
        payload.insert(payload.end(), trex.begin(), trex.end());
    }
    return make_box("mvex", payload);
}

std::vector<std::uint8_t> build_init_segment_from_tracks(const std::vector<TrackDescription>& tracks) {
    auto ftyp = build_ftyp_box();
    auto mvhd = build_mvhd_box();
    auto mvex = build_mvex_box(tracks);

    std::vector<std::uint8_t> moov_payload;
    moov_payload.insert(moov_payload.end(), mvhd.begin(), mvhd.end());
    for (const auto& track : tracks) {
        auto trak = build_trak_box(track);
        moov_payload.insert(moov_payload.end(), trak.begin(), trak.end());
    }
    moov_payload.insert(moov_payload.end(), mvex.begin(), mvex.end());

    auto moov = make_box("moov", moov_payload);
    std::vector<std::uint8_t> init;
    init.insert(init.end(), ftyp.begin(), ftyp.end());
    init.insert(init.end(), moov.begin(), moov.end());
    return init;
}

bool h264_annexb_has_idr(std::span<const std::uint8_t> payload) {
    for (std::size_t i = 0; i + 4 < payload.size(); ++i) {
        if (payload[i] == 0x00 && payload[i + 1] == 0x00 &&
            ((payload[i + 2] == 0x01) || (payload[i + 2] == 0x00 && payload[i + 3] == 0x01))) {
            const std::size_t nal_offset = payload[i + 2] == 0x01 ? i + 3 : i + 4;
            if (nal_offset < payload.size()) {
                const std::uint8_t nal_type = static_cast<std::uint8_t>(payload[nal_offset] & 0x1FU);
                if (nal_type == 5) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::vector<std::uint8_t> annexb_to_avcc(std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> out;
    std::size_t i = 0;
    while (i + 3 < payload.size()) {
        std::size_t start = std::string::npos;
        std::size_t sc_len = 0;
        for (; i + 3 < payload.size(); ++i) {
            if (payload[i] == 0x00 && payload[i + 1] == 0x00 && payload[i + 2] == 0x01) {
                start = i + 3;
                sc_len = 3;
                break;
            }
            if (i + 4 < payload.size() && payload[i] == 0x00 && payload[i + 1] == 0x00 &&
                payload[i + 2] == 0x00 && payload[i + 3] == 0x01) {
                start = i + 4;
                sc_len = 4;
                break;
            }
        }
        if (start == std::string::npos || start >= payload.size()) {
            break;
        }
        std::size_t next = start;
        while (next + 3 < payload.size()) {
            if (payload[next] == 0x00 && payload[next + 1] == 0x00 &&
                (payload[next + 2] == 0x01 ||
                 (next + 3 < payload.size() && payload[next + 2] == 0x00 && payload[next + 3] == 0x01))) {
                break;
            }
            ++next;
        }
        if (next + 3 >= payload.size()) {
            next = payload.size();
        }
        const std::size_t nal_size = next - start;
        if (nal_size == 0) {
            i = next;
            continue;
        }
        append_be32(out, static_cast<std::uint32_t>(nal_size));
        out.insert(out.end(), payload.begin() + static_cast<std::ptrdiff_t>(start),
                   payload.begin() + static_cast<std::ptrdiff_t>(start + nal_size));
        i = next;
        if (sc_len == 0) {
            break;
        }
    }
    if (out.empty()) {
        return std::vector<std::uint8_t>(payload.begin(), payload.end());
    }
    return out;
}

struct EsSample {
    bool is_video = false;
    std::uint64_t pts90k = 0;
    std::uint8_t stream_type = 0;
    std::vector<std::uint8_t> payload;
    bool keyframe = false;
    // For audio from ADTS: first 9 bytes of the original ADTS header (for codec discovery).
    // Empty for video or non-ADTS audio, or after the first frame in a multi-frame PES.
    std::array<std::uint8_t, 9> adts_header{};
    std::uint8_t adts_header_len = 0;
};

enum class VideoCodec {
    kUnknown,
    kH264,
    kHevc,
};

struct CallerTrackState {
    std::string video_track_name;
    std::string audio_track_name;
    std::uint32_t video_track_id = 0;
    std::uint32_t audio_track_id = 0;
    std::uint32_t video_timescale = 90000;
    std::uint32_t audio_timescale = 48000;
    std::size_t group_id = 0;
    bool first_video_keyframe_seen = false;
    std::map<std::string, std::size_t> object_id_by_track;
    std::map<std::string, std::uint64_t> last_pts_by_track;
    std::map<std::string, std::uint64_t> last_duration_us_by_track;
    std::map<std::string, std::uint64_t> decode_time_by_track;
    std::uint32_t moof_sequence = 1;
    VideoCodec video_codec = VideoCodec::kUnknown;
    // Shared PTS origin (90 kHz) for A/V timeline alignment.
    // Set once from the first sample received (video or audio).
    std::optional<std::uint64_t> base_pts90k;
};

VideoCodec detect_video_codec_from_stream_type(std::uint8_t stream_type) {
    if (stream_type == 0x1B || stream_type == 0x02) {
        return VideoCodec::kH264;
    }
    if (stream_type == 0x24) {
        return VideoCodec::kHevc;
    }
    return VideoCodec::kUnknown;
}

VideoCodec detect_video_codec_from_annexb(std::span<const std::uint8_t> payload) {
    bool saw_h264_marker = false;
    for (std::size_t i = 0; i + 4 < payload.size(); ++i) {
        if (payload[i] == 0x00 && payload[i + 1] == 0x00 &&
            (payload[i + 2] == 0x01 ||
             (i + 4 < payload.size() && payload[i + 2] == 0x00 && payload[i + 3] == 0x01))) {
            const std::size_t nal_offset = payload[i + 2] == 0x01 ? i + 3 : i + 4;
            if (nal_offset >= payload.size()) {
                continue;
            }
            const std::uint8_t h264_type = static_cast<std::uint8_t>(payload[nal_offset] & 0x1FU);
            if (h264_type == 7 || h264_type == 8 || h264_type == 5) {
                saw_h264_marker = true;
            }
            const std::uint8_t hevc_type = static_cast<std::uint8_t>((payload[nal_offset] >> 1U) & 0x3FU);
            if (hevc_type == 32 || hevc_type == 33 || hevc_type == 34 ||
                hevc_type == 19 || hevc_type == 20 || hevc_type == 21) {
                return VideoCodec::kHevc;
            }
        }
    }
    return saw_h264_marker ? VideoCodec::kH264 : VideoCodec::kUnknown;
}

bool hevc_annexb_has_irap(std::span<const std::uint8_t> payload) {
    for (std::size_t i = 0; i + 4 < payload.size(); ++i) {
        if (payload[i] == 0x00 && payload[i + 1] == 0x00 &&
            (payload[i + 2] == 0x01 ||
             (i + 4 < payload.size() && payload[i + 2] == 0x00 && payload[i + 3] == 0x01))) {
            const std::size_t nal_offset = payload[i + 2] == 0x01 ? i + 3 : i + 4;
            if (nal_offset < payload.size()) {
                const std::uint8_t nal_type = static_cast<std::uint8_t>((payload[nal_offset] >> 1U) & 0x3FU);
                if (nal_type >= 16 && nal_type <= 23) {
                    return true;
                }
            }
        }
    }
    return false;
}

// Extract individual NAL units from AnnexB-formatted H.264/HEVC bitstream.
std::vector<std::vector<std::uint8_t>> extract_nal_units(std::span<const std::uint8_t> payload) {
    std::vector<std::vector<std::uint8_t>> nals;
    std::size_t i = 0;
    while (i + 3 < payload.size()) {
        std::size_t start = std::string::npos;
        for (; i + 3 < payload.size(); ++i) {
            if (payload[i] == 0x00 && payload[i + 1] == 0x00) {
                if (payload[i + 2] == 0x01) {
                    start = i + 3;
                    i = start;
                    break;
                }
                if (i + 3 < payload.size() && payload[i + 2] == 0x00 && payload[i + 3] == 0x01) {
                    start = i + 4;
                    i = start;
                    break;
                }
            }
        }
        if (start == std::string::npos || start >= payload.size()) {
            break;
        }
        // Find next start code or end of data
        std::size_t end = start;
        while (end + 3 < payload.size()) {
            if (payload[end] == 0x00 && payload[end + 1] == 0x00 &&
                (payload[end + 2] == 0x01 ||
                 (end + 3 < payload.size() && payload[end + 2] == 0x00 && payload[end + 3] == 0x01))) {
                break;
            }
            ++end;
        }
        if (end == start && end + 3 >= payload.size()) {
            end = payload.size();
        } else if (end == start) {
            continue;
        }
        // Trim trailing zeros from NAL unit
        std::size_t trimmed_end = end;
        while (trimmed_end > start && payload[trimmed_end - 1] == 0x00) {
            --trimmed_end;
        }
        if (trimmed_end > start) {
            nals.emplace_back(payload.begin() + static_cast<std::ptrdiff_t>(start),
                              payload.begin() + static_cast<std::ptrdiff_t>(trimmed_end));
        }
        i = end;
    }
    return nals;
}

// Build an avcC box (AVCDecoderConfigurationRecord) from SPS and PPS NAL units.
// Returns the complete box including the 4-byte type header.
std::vector<std::uint8_t> build_avcc_box(const std::vector<std::uint8_t>& sps,
                                         const std::vector<std::uint8_t>& pps) {
    if (sps.size() < 4 || pps.empty()) {
        return {};
    }
    // AVCDecoderConfigurationRecord
    std::vector<std::uint8_t> record;
    record.push_back(1);                      // configurationVersion
    record.push_back(sps[1]);                 // AVCProfileIndication
    record.push_back(sps[2]);                 // profile_compatibility
    record.push_back(sps[3]);                 // AVCLevelIndication
    record.push_back(0xFF);                   // lengthSizeMinusOne = 3 (4-byte NAL lengths) | reserved 6 bits
    record.push_back(static_cast<std::uint8_t>(0xE0U | 1U));  // numOfSequenceParameterSets = 1 | reserved 3 bits
    append_be16(record, static_cast<std::uint16_t>(sps.size()));
    record.insert(record.end(), sps.begin(), sps.end());
    record.push_back(1);                      // numOfPictureParameterSets
    append_be16(record, static_cast<std::uint16_t>(pps.size()));
    record.insert(record.end(), pps.begin(), pps.end());
    // Wrap in box
    return make_box("avcC", record);
}

// Extract VPS, SPS, PPS from AnnexB HEVC keyframe.
struct HevcParamSets {
    std::vector<std::uint8_t> vps;
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;
};

HevcParamSets extract_hevc_param_sets(std::span<const std::uint8_t> payload) {
    HevcParamSets params;
    auto nals = extract_nal_units(payload);
    for (const auto& nal : nals) {
        if (nal.size() < 2) continue;
        const std::uint8_t nal_type = static_cast<std::uint8_t>((nal[0] >> 1U) & 0x3FU);
        if (nal_type == 32 && params.vps.empty()) {  // VPS
            params.vps = nal;
        } else if (nal_type == 33 && params.sps.empty()) {  // SPS
            params.sps = nal;
        } else if (nal_type == 34 && params.pps.empty()) {  // PPS
            params.pps = nal;
        }
        if (!params.vps.empty() && !params.sps.empty() && !params.pps.empty()) break;
    }
    return params;
}

// Minimal RBSP bitstream reader used only for SPS dimension extraction.
class RbspBitReader {
public:
    RbspBitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool read_bit(std::uint8_t& out) {
        if (byte_pos_ >= size_) return false;
        out = static_cast<std::uint8_t>((data_[byte_pos_] >> (7U - bit_pos_)) & 0x01U);
        if (++bit_pos_ == 8U) { bit_pos_ = 0; ++byte_pos_; }
        return true;
    }

    bool skip_bits(std::size_t count) {
        for (std::size_t k = 0; k < count; ++k) {
            std::uint8_t b = 0;
            if (!read_bit(b)) return false;
        }
        return true;
    }

    bool read_ue(std::uint32_t& out) {
        int zeros = 0;
        std::uint8_t bit = 0;
        while (zeros < 32) {
            if (!read_bit(bit)) return false;
            if (bit != 0) break;
            ++zeros;
        }
        if (zeros == 32) return false;
        std::uint32_t suffix = 0;
        for (int k = 0; k < zeros; ++k) {
            if (!read_bit(bit)) return false;
            suffix = (suffix << 1U) | bit;
        }
        out = (1U << zeros) - 1U + suffix;
        return true;
    }

    bool skip_ue() { std::uint32_t v = 0; return read_ue(v); }

    bool read_se(std::int32_t& out) {
        std::uint32_t ue = 0;
        if (!read_ue(ue)) return false;
        out = (ue % 2U == 0U) ? -static_cast<std::int32_t>(ue / 2U)
                               : static_cast<std::int32_t>((ue + 1U) / 2U);
        return true;
    }

    bool skip_se() { std::int32_t v = 0; return read_se(v); }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t byte_pos_ = 0;
    std::uint8_t bit_pos_ = 0;
};

// Parse {width, height} from a raw H.264 SPS NAL unit (includes 1-byte NAL header).
// Returns {0, 0} on any parse failure.
std::pair<std::uint32_t, std::uint32_t>
parse_h264_dimensions(const std::vector<std::uint8_t>& sps_nal) {
    if (sps_nal.size() < 5) return {0, 0};
    const std::uint8_t profile_idc = sps_nal[1];
    RbspBitReader r(sps_nal.data() + 4, sps_nal.size() - 4);

    if (!r.skip_ue()) return {0, 0};  // seq_parameter_set_id

    std::uint32_t chroma_format_idc = 1;
    static constexpr std::uint8_t kHighProfiles[] = {
        100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135
    };
    for (std::uint8_t hp : kHighProfiles) {
        if (profile_idc != hp) continue;
        if (!r.read_ue(chroma_format_idc)) return {0, 0};
        if (chroma_format_idc == 3 && !r.skip_bits(1)) return {0, 0};
        if (!r.skip_ue()) return {0, 0};  // bit_depth_luma_minus8
        if (!r.skip_ue()) return {0, 0};  // bit_depth_chroma_minus8
        if (!r.skip_bits(1)) return {0, 0};  // qpprime_y_zero_transform_bypass_flag
        std::uint8_t ssmf = 0;
        if (!r.read_bit(ssmf)) return {0, 0};
        if (ssmf) {
            const int n_lists = (chroma_format_idc != 3) ? 8 : 12;
            for (int j = 0; j < n_lists; ++j) {
                std::uint8_t present = 0;
                if (!r.read_bit(present)) return {0, 0};
                if (present) {
                    const int sl_size = (j < 6) ? 16 : 64;
                    int last = 8, next = 8;
                    for (int k = 0; k < sl_size; ++k) {
                        if (next != 0) {
                            std::int32_t delta = 0;
                            if (!r.read_se(delta)) return {0, 0};
                            next = (last + delta + 256) % 256;
                        }
                        last = (next == 0) ? last : next;
                    }
                }
            }
        }
        break;
    }

    if (!r.skip_ue()) return {0, 0};  // log2_max_frame_num_minus4
    std::uint32_t poc_type = 0;
    if (!r.read_ue(poc_type)) return {0, 0};
    if (poc_type == 0) {
        if (!r.skip_ue()) return {0, 0};
    } else if (poc_type == 1) {
        if (!r.skip_bits(1) || !r.skip_se() || !r.skip_se()) return {0, 0};
        std::uint32_t num_ref = 0;
        if (!r.read_ue(num_ref)) return {0, 0};
        for (std::uint32_t k = 0; k < num_ref; ++k) {
            if (!r.skip_se()) return {0, 0};
        }
    }
    if (!r.skip_ue()) return {0, 0};  // max_num_ref_frames
    if (!r.skip_bits(1)) return {0, 0};  // gaps_in_frame_num_value_allowed_flag

    std::uint32_t pic_w = 0, pic_h = 0;
    if (!r.read_ue(pic_w) || !r.read_ue(pic_h)) return {0, 0};
    std::uint8_t frame_mbs_only = 0;
    if (!r.read_bit(frame_mbs_only)) return {0, 0};
    const std::uint32_t width = (pic_w + 1U) * 16U;
    std::uint32_t height = (pic_h + 1U) * 16U * (2U - frame_mbs_only);

    if (!frame_mbs_only && !r.skip_bits(1)) return {width, height};
    if (!r.skip_bits(1)) return {width, height};  // direct_8x8_inference_flag
    std::uint8_t crop = 0;
    if (!r.read_bit(crop)) return {width, height};
    if (crop) {
        const std::uint32_t cux = (chroma_format_idc == 0) ? 1U : 2U;
        const std::uint32_t cuy = (chroma_format_idc == 0) ? (2U - frame_mbs_only)
                                                            : (2U * (2U - frame_mbs_only));
        std::uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
        if (!r.read_ue(cl) || !r.read_ue(cr) || !r.read_ue(ct) || !r.read_ue(cb)) {
            return {width, height};
        }
        return {width - (cl + cr) * cux, height - (ct + cb) * cuy};
    }
    return {width, height};
}

// Parse {width, height} from a raw HEVC SPS NAL unit (includes 2-byte NAL header).
// Returns {0, 0} on any parse failure.
std::pair<std::uint32_t, std::uint32_t>
parse_hevc_dimensions(const std::vector<std::uint8_t>& sps_nal) {
    if (sps_nal.size() < 16) return {0, 0};
    const std::uint8_t max_sub_layers_minus1 = static_cast<std::uint8_t>((sps_nal[2] >> 1U) & 0x07U);
    RbspBitReader r(sps_nal.data() + 2, sps_nal.size() - 2);
    if (!r.skip_bits(8)) return {0, 0};   // vps_id(4)+max_sub_layers(3)+temporal_nesting(1)
    if (!r.skip_bits(96)) return {0, 0};  // profile_tier_level general_* (12 bytes)

    bool sub_profile[8] = {}, sub_level[8] = {};
    for (int j = 0; j < static_cast<int>(max_sub_layers_minus1); ++j) {
        std::uint8_t b = 0;
        if (!r.read_bit(b)) return {0, 0};
        sub_profile[j] = (b != 0);
        if (!r.read_bit(b)) return {0, 0};
        sub_level[j] = (b != 0);
    }
    if (max_sub_layers_minus1 < 8 && !r.skip_bits(2U * (8U - max_sub_layers_minus1))) return {0, 0};
    for (int j = 0; j < static_cast<int>(max_sub_layers_minus1); ++j) {
        if (sub_profile[j] && !r.skip_bits(88)) return {0, 0};
        if (sub_level[j]   && !r.skip_bits(8))  return {0, 0};
    }

    if (!r.skip_ue()) return {0, 0};  // sps_seq_parameter_set_id
    std::uint32_t chroma_format_idc = 0;
    if (!r.read_ue(chroma_format_idc)) return {0, 0};
    if (chroma_format_idc == 3 && !r.skip_bits(1)) return {0, 0};

    std::uint32_t width = 0, height = 0;
    if (!r.read_ue(width) || !r.read_ue(height)) return {0, 0};

    std::uint8_t conf_win = 0;
    if (!r.read_bit(conf_win)) return {width, height};
    if (conf_win) {
        const std::uint32_t swc = (chroma_format_idc == 1 || chroma_format_idc == 2) ? 2U : 1U;
        const std::uint32_t shc = (chroma_format_idc == 1) ? 2U : 1U;
        std::uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
        if (!r.read_ue(cl) || !r.read_ue(cr) || !r.read_ue(ct) || !r.read_ue(cb)) {
            return {width, height};
        }
        width  -= (cl + cr) * swc;
        height -= (ct + cb) * shc;
    }
    return {width, height};
}

// Build an hvcC box (HEVCDecoderConfigurationRecord) from VPS, SPS, PPS.
std::vector<std::uint8_t> build_hvcc_box(const HevcParamSets& params) {
    if (params.vps.empty() || params.sps.size() < 2 || params.pps.empty()) {
        return {};
    }

    // Parse basic info from SPS NAL (after the 2-byte NAL header)
    // SPS: nal_header(2) + sps_video_parameter_set_id(4b) + sps_max_sub_layers_minus1(3b) + temporal_id_nesting(1b) + profile_tier_level(...)
    const std::uint8_t* sps_data = params.sps.data() + 2;  // skip 2-byte NAL header
    const std::size_t sps_payload_size = params.sps.size() - 2;
    if (sps_payload_size < 13) {
        return {};
    }

    // profile_tier_level starts at byte 0 of sps_data after 4-bit vps_id + 3-bit max_sub_layers + 1-bit temporal nesting
    // But we need: general_profile_space(2b) + general_tier_flag(1b) + general_profile_idc(5b) = 1 byte
    // general_profile_compatibility_flags(32b) = 4 bytes
    // constraint_indicator_flags(48b) = 6 bytes
    // general_level_idc(8b) = 1 byte
    // Total profile_tier_level fixed part = 12 bytes starting after the first byte
    const std::uint8_t profile_byte = sps_data[1];
    const std::uint8_t general_profile_space = static_cast<std::uint8_t>((profile_byte >> 6U) & 0x03U);
    const std::uint8_t general_tier_flag = static_cast<std::uint8_t>((profile_byte >> 5U) & 0x01U);
    const std::uint8_t general_profile_idc = static_cast<std::uint8_t>(profile_byte & 0x1FU);
    const std::uint32_t general_profile_compat = (static_cast<std::uint32_t>(sps_data[2]) << 24U) |
                                                  (static_cast<std::uint32_t>(sps_data[3]) << 16U) |
                                                  (static_cast<std::uint32_t>(sps_data[4]) << 8U) |
                                                  static_cast<std::uint32_t>(sps_data[5]);
    // constraint_indicator_flags: 6 bytes
    std::uint8_t constraint_flags[6];
    for (int j = 0; j < 6; ++j) {
        constraint_flags[j] = sps_data[6 + j];
    }
    const std::uint8_t general_level_idc = sps_data[12];

    // Max sub layers from first byte of SPS payload: bits[4:6] = sps_max_sub_layers_minus1
    const std::uint8_t max_sub_layers = static_cast<std::uint8_t>((sps_data[0] >> 1U) & 0x07U);

    // Build HEVCDecoderConfigurationRecord (ISO 14496-15 section 8.3.3.1.2)
    std::vector<std::uint8_t> record;
    record.push_back(1);  // configurationVersion
    record.push_back(static_cast<std::uint8_t>((general_profile_space << 6U) | (general_tier_flag << 5U) | general_profile_idc));
    append_be32(record, general_profile_compat);
    record.insert(record.end(), constraint_flags, constraint_flags + 6);
    record.push_back(general_level_idc);
    // min_spatial_segmentation_idc = 0, with reserved bits
    append_be16(record, 0xF000U);
    // parallelismType = 0 with reserved bits
    record.push_back(0xFCU);
    // chromaFormat = 1 (4:2:0) with reserved bits
    record.push_back(0xFDU);
    // bitDepthLumaMinus8 = 0 with reserved bits
    record.push_back(0xF8U);
    // bitDepthChromaMinus8 = 0 with reserved bits
    record.push_back(0xF8U);
    // avgFrameRate = 0
    append_be16(record, 0);
    // constantFrameRate(2b)=0 + numTemporalLayers(3b) + temporalIdNested(1b) + lengthSizeMinusOne(2b)=3
    record.push_back(static_cast<std::uint8_t>(((max_sub_layers + 1) << 3U) | 0x04U | 0x03U));
    // numOfArrays = 3 (VPS, SPS, PPS)
    record.push_back(3);

    // Array entry helper: array_completeness(1b)=1 + reserved(1b)=0 + NAL_unit_type(6b), numNalus, nalUnitLength, nalUnit
    auto write_array = [&](std::uint8_t nal_type, const std::vector<std::uint8_t>& nal) {
        record.push_back(static_cast<std::uint8_t>(0x80U | nal_type));  // array_completeness=1
        append_be16(record, 1);  // numNalus
        append_be16(record, static_cast<std::uint16_t>(nal.size()));
        record.insert(record.end(), nal.begin(), nal.end());
    };

    write_array(32, params.vps);  // VPS
    write_array(33, params.sps);  // SPS
    write_array(34, params.pps);  // PPS

    return make_box("hvcC", record);
}

// Build an esds box for AAC audio from ADTS header bytes.
// adts_header must be at least 7 bytes (a valid ADTS fixed header).
// Uses expanded (4-byte) descriptor length encoding to match ffmpeg/reference format.
std::vector<std::uint8_t> build_esds_box(std::span<const std::uint8_t> adts_header) {
    if (adts_header.size() < 7) {
        return {};
    }
    // Parse ADTS fixed header fields
    const std::uint8_t profile = static_cast<std::uint8_t>((adts_header[2] >> 6U) & 0x03U);  // 0=Main, 1=LC, 2=SSR, 3=LTP
    const std::uint8_t freq_index = static_cast<std::uint8_t>((adts_header[2] >> 2U) & 0x0FU);
    const std::uint8_t channel_config = static_cast<std::uint8_t>(((adts_header[2] & 0x01U) << 2U) |
                                                                   ((adts_header[3] >> 6U) & 0x03U));

    // Build AudioSpecificConfig (2 bytes for LC-AAC)
    const std::uint8_t audio_object_type = static_cast<std::uint8_t>(profile + 1);  // AOT = profile + 1
    std::uint8_t asc[2];
    asc[0] = static_cast<std::uint8_t>((audio_object_type << 3U) | (freq_index >> 1U));
    asc[1] = static_cast<std::uint8_t>((freq_index << 7U) | (channel_config << 3U));

    // Helper: write descriptor tag + expanded 4-byte length (matches ffmpeg/ISO format)
    auto write_descr_tag = [](std::vector<std::uint8_t>& out, std::uint8_t tag, std::uint32_t length) {
        out.push_back(tag);
        out.push_back(static_cast<std::uint8_t>(0x80U | ((length >> 21U) & 0x7FU)));
        out.push_back(static_cast<std::uint8_t>(0x80U | ((length >> 14U) & 0x7FU)));
        out.push_back(static_cast<std::uint8_t>(0x80U | ((length >> 7U) & 0x7FU)));
        out.push_back(static_cast<std::uint8_t>(length & 0x7FU));
    };

    // Compute descriptor body sizes (without tag+length overhead)
    const std::uint32_t dec_specific_body = 2;  // AudioSpecificConfig
    const std::uint32_t dec_config_body = 13 + 5 + dec_specific_body;  // 13 fixed + tag(1)+len(4) + ASC
    const std::uint32_t sl_config_body = 1;   // predefined
    const std::uint32_t es_desc_body = 3 + 5 + dec_config_body + 5 + sl_config_body;  // ES_ID(2)+flags(1) + tag+len+DecConfig + tag+len+SLConfig

    // Build the full esds atom content
    std::vector<std::uint8_t> esds_content;
    esds_content.push_back(0);  // version
    esds_content.push_back(0);  // flags[0]
    esds_content.push_back(0);  // flags[1]
    esds_content.push_back(0);  // flags[2]

    // ES_Descriptor (tag=0x03)
    write_descr_tag(esds_content, 0x03, es_desc_body);
    append_be16(esds_content, 0x0001);  // ES_ID = 1
    esds_content.push_back(0x00);       // streamDependenceFlag=0 URL_Flag=0 OCRstreamFlag=0 streamPriority=0

    // DecoderConfigDescriptor (tag=0x04)
    write_descr_tag(esds_content, 0x04, dec_config_body);
    esds_content.push_back(0x40);  // objectTypeIndication = Audio ISO/IEC 14496-3
    esds_content.push_back(0x15);  // streamType=5 (audio) upstream=0 reserved=1
    esds_content.push_back(0x00);  // bufferSizeDB[0]
    esds_content.push_back(0x00);  // bufferSizeDB[1]
    esds_content.push_back(0x00);  // bufferSizeDB[2]
    append_be32(esds_content, 128000);  // maxBitrate
    append_be32(esds_content, 128000);  // avgBitrate

    // DecoderSpecificInfo (tag=0x05)
    write_descr_tag(esds_content, 0x05, dec_specific_body);
    esds_content.push_back(asc[0]);
    esds_content.push_back(asc[1]);

    // SLConfigDescriptor (tag=0x06)
    write_descr_tag(esds_content, 0x06, sl_config_body);
    esds_content.push_back(0x02);  // predefined = 2

    return make_box("esds", esds_content);
}

// Extract SPS and PPS from AnnexB H.264 keyframe.
// Returns {sps, pps} pair. Either may be empty if not found.
std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>
extract_h264_sps_pps(std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> sps, pps;
    auto nals = extract_nal_units(payload);
    for (const auto& nal : nals) {
        if (nal.empty()) continue;
        const std::uint8_t nal_type = static_cast<std::uint8_t>(nal[0] & 0x1FU);
        if (nal_type == 7 && sps.empty()) {  // SPS
            sps = nal;
        } else if (nal_type == 8 && pps.empty()) {  // PPS
            pps = nal;
        }
        if (!sps.empty() && !pps.empty()) break;
    }
    return {sps, pps};
}

// Build codec_private bytes for H.264 track from first keyframe's AnnexB data.
std::vector<std::uint8_t> build_h264_codec_private(std::span<const std::uint8_t> annexb_keyframe) {
    auto [sps, pps] = extract_h264_sps_pps(annexb_keyframe);
    return build_avcc_box(sps, pps);
}

// Build codec_private bytes for AAC from first ADTS frame.
// ADTS sampling frequency table (ISO 14496-3)
constexpr std::uint32_t kAdtsSampleRates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000, 7350
};

// Parse ADTS header to extract sample_rate and channel_count.
// Returns {sample_rate, channel_count} or {0,0} on failure.
std::pair<std::uint32_t, std::uint32_t> parse_adts_audio_params(std::span<const std::uint8_t> adts_header) {
    if (adts_header.size() < 7) return {0, 0};
    const std::uint8_t freq_index = static_cast<std::uint8_t>((adts_header[2] >> 2U) & 0x0FU);
    const std::uint8_t channel_config = static_cast<std::uint8_t>(((adts_header[2] & 0x01U) << 2U) |
                                                                   ((adts_header[3] >> 6U) & 0x03U));
    const std::uint32_t sample_rate = freq_index < 13 ? kAdtsSampleRates[freq_index] : 0;
    // channel_config 1-7 maps directly to channel count (1=mono, 2=stereo, ..., 6=5.1, 7=7.1)
    const std::uint32_t channels = channel_config <= 7 ? channel_config : 0;
    return {sample_rate, channels};
}

std::vector<std::uint8_t> build_moof_box(std::uint32_t sequence_number,
                                         std::uint32_t track_id,
                                         std::uint64_t base_decode_time,
                                         std::uint32_t sample_duration,
                                         std::uint32_t sample_size,
                                         std::uint32_t sample_flags,
                                         std::int32_t sample_cts_offset,
                                         bool default_base_moof) {
    std::vector<std::uint8_t> mfhd_payload;
    append_be32(mfhd_payload, sequence_number);
    auto mfhd = make_full_box("mfhd", 0, 0, mfhd_payload);

    std::vector<std::uint8_t> tfhd_payload;
    append_be32(tfhd_payload, track_id);
    std::uint32_t tfhd_flags = default_base_moof ? 0x020000U : 0U;
    auto tfhd = make_full_box("tfhd", 0, tfhd_flags, tfhd_payload);

    std::vector<std::uint8_t> tfdt_payload;
    append_be64(tfdt_payload, base_decode_time);
    auto tfdt = make_full_box("tfdt", 1, 0, tfdt_payload);

    std::vector<std::uint8_t> trun_payload;
    append_be32(trun_payload, 1);
    append_be32(trun_payload, 0);
    append_be32(trun_payload, sample_duration);
    append_be32(trun_payload, sample_size);
    append_be32(trun_payload, sample_flags);
    append_be32(trun_payload, static_cast<std::uint32_t>(sample_cts_offset));
    auto trun = make_full_box("trun", 0, 0x000F01U, trun_payload);

    std::vector<std::uint8_t> traf_payload;
    traf_payload.insert(traf_payload.end(), tfhd.begin(), tfhd.end());
    traf_payload.insert(traf_payload.end(), tfdt.begin(), tfdt.end());
    traf_payload.insert(traf_payload.end(), trun.begin(), trun.end());
    auto traf = make_box("traf", traf_payload);

    std::vector<std::uint8_t> moof_payload;
    moof_payload.insert(moof_payload.end(), mfhd.begin(), mfhd.end());
    moof_payload.insert(moof_payload.end(), traf.begin(), traf.end());
    auto moof = make_box("moof", moof_payload);

    const std::uint32_t trun_data_offset = static_cast<std::uint32_t>(moof.size() + 8);
    const std::size_t trun_offset = moof.size() - trun.size();
    const std::size_t data_offset_field = trun_offset + 8 + 4 + 4;
    moof[data_offset_field + 0] = static_cast<std::uint8_t>((trun_data_offset >> 24U) & 0xFFU);
    moof[data_offset_field + 1] = static_cast<std::uint8_t>((trun_data_offset >> 16U) & 0xFFU);
    moof[data_offset_field + 2] = static_cast<std::uint8_t>((trun_data_offset >> 8U) & 0xFFU);
    moof[data_offset_field + 3] = static_cast<std::uint8_t>(trun_data_offset & 0xFFU);

    return moof;
}

std::vector<std::uint8_t> build_mdat_box(std::span<const std::uint8_t> sample_bytes) {
    return make_box("mdat", sample_bytes);
}

class TsPesDemuxer {
public:
    explicit TsPesDemuxer(const LiveSrtCallerRuntimeConfig& config)
        : config_(config) {
        if (config_.has_video_pid) {
            video_pid_ = static_cast<std::uint16_t>(config_.video_pid & 0x1FFFU);
        }
        if (config_.has_audio_pid) {
            audio_pid_ = static_cast<std::uint16_t>(config_.audio_pid & 0x1FFFU);
        }
    }

    void feed(const std::uint8_t* data,
              std::size_t size,
              const std::function<void(EsSample&&)>& sample_sink) {
        buffer_.insert(buffer_.end(), data, data + static_cast<std::ptrdiff_t>(size));

        while (buffer_.size() >= 188) {
            if (buffer_[0] != 0x47) {
                auto sync_it = std::find(buffer_.begin() + 1, buffer_.end(), 0x47);
                if (sync_it == buffer_.end()) {
                    buffer_.clear();
                    return;
                }
                buffer_.erase(buffer_.begin(), sync_it);
                continue;
            }

            std::array<std::uint8_t, 188> packet{};
            std::copy_n(buffer_.begin(), 188, packet.begin());
            buffer_.erase(buffer_.begin(), buffer_.begin() + 188);
            parse_packet(packet, sample_sink);
        }
    }

private:
    struct PesBuffer {
        bool active = false;
        bool is_video = false;
        std::uint64_t pts90k = 0;
        std::vector<std::uint8_t> data;
    };

    void parse_packet(std::span<const std::uint8_t, 188> packet,
                      const std::function<void(EsSample&&)>& sample_sink) {
        const bool payload_unit_start = (packet[1] & 0x40U) != 0;
        const std::uint16_t pid = static_cast<std::uint16_t>(((packet[1] & 0x1FU) << 8U) | packet[2]);
        const std::uint8_t adaptation_control = static_cast<std::uint8_t>((packet[3] >> 4U) & 0x03U);
        if (adaptation_control == 0 || adaptation_control == 2) {
            return;
        }

        std::size_t offset = 4;
        if (adaptation_control == 3) {
            const std::size_t adaptation_len = packet[offset];
            offset += 1 + adaptation_len;
            if (offset >= packet.size()) {
                return;
            }
        }

        if (pid == 0x0000U) {
            parse_pat(packet.subspan(offset), payload_unit_start);
            return;
        }
        if (pmt_pid_.has_value() && pid == *pmt_pid_) {
            parse_pmt(packet.subspan(offset), payload_unit_start);
            return;
        }
        if (video_pid_.has_value() && pid == *video_pid_) {
            parse_pes(packet.subspan(offset), payload_unit_start, pid, true, sample_sink);
            return;
        }
        if (audio_pid_.has_value() && pid == *audio_pid_) {
            parse_pes(packet.subspan(offset), payload_unit_start, pid, false, sample_sink);
        }
    }

    void parse_pat(std::span<const std::uint8_t> payload, bool payload_unit_start) {
        if (!payload_unit_start || payload.empty()) {
            return;
        }
        const std::size_t pointer = payload[0];
        if (1 + pointer + 8 > payload.size()) {
            return;
        }
        const std::size_t sec = 1 + pointer;
        if (payload[sec] != 0x00) {
            return;
        }
        const std::uint16_t section_length = static_cast<std::uint16_t>(((payload[sec + 1] & 0x0FU) << 8U) | payload[sec + 2]);
        const std::size_t end = sec + 3 + section_length;
        if (end > payload.size() || section_length < 9) {
            return;
        }
        for (std::size_t cursor = sec + 8; cursor + 4 <= end - 4; cursor += 4) {
            const std::uint16_t program_number = read_be16(payload, cursor);
            const std::uint16_t pid = static_cast<std::uint16_t>(((payload[cursor + 2] & 0x1FU) << 8U) | payload[cursor + 3]);
            if (program_number == 0) {
                continue;
            }
            if (!config_.has_program_number || program_number == config_.program_number) {
                pmt_pid_ = pid;
                return;
            }
        }
    }

    void parse_pmt(std::span<const std::uint8_t> payload, bool payload_unit_start) {
        if (!payload_unit_start || payload.empty()) {
            return;
        }
        const std::size_t pointer = payload[0];
        if (1 + pointer + 12 > payload.size()) {
            return;
        }
        const std::size_t sec = 1 + pointer;
        if (payload[sec] != 0x02) {
            return;
        }
        const std::uint16_t section_length = static_cast<std::uint16_t>(((payload[sec + 1] & 0x0FU) << 8U) | payload[sec + 2]);
        const std::size_t end = sec + 3 + section_length;
        if (end > payload.size() || section_length < 13) {
            return;
        }

        const std::uint16_t program_info_length = static_cast<std::uint16_t>(((payload[sec + 10] & 0x0FU) << 8U) | payload[sec + 11]);
        std::size_t cursor = sec + 12 + program_info_length;
        while (cursor + 5 <= end - 4) {
            const std::uint8_t stream_type = payload[cursor];
            const std::uint16_t elementary_pid =
                static_cast<std::uint16_t>(((payload[cursor + 1] & 0x1FU) << 8U) | payload[cursor + 2]);
            const std::uint16_t es_info_len =
                static_cast<std::uint16_t>(((payload[cursor + 3] & 0x0FU) << 8U) | payload[cursor + 4]);

            if (!video_pid_.has_value() && (stream_type == 0x1B || stream_type == 0x24 || stream_type == 0x02)) {
                video_pid_ = elementary_pid;
                video_stream_type_ = stream_type;
            }
            if (!audio_pid_.has_value() && (stream_type == 0x0F || stream_type == 0x11 || stream_type == 0x03 || stream_type == 0x04)) {
                audio_pid_ = elementary_pid;
                audio_stream_type_ = stream_type;
            }
            cursor += 5 + es_info_len;
        }
        pmt_parsed_ = true;
    }

    void parse_pes(std::span<const std::uint8_t> payload,
                   bool payload_unit_start,
                   std::uint16_t pid,
                   bool is_video,
                   const std::function<void(EsSample&&)>& sample_sink) {
        auto& pes = pes_by_pid_[pid];
        if (payload_unit_start) {
            flush_pes(pes, sample_sink);
            pes = PesBuffer{};
            pes.active = true;
            pes.is_video = is_video;
            if (payload.size() < 9) {
                return;
            }
            if (!(payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01)) {
                return;
            }
            const std::uint8_t flags = payload[7];
            const std::uint8_t header_len = payload[8];
            std::size_t cursor = 9;
            if ((flags & 0x80U) != 0 && cursor + 5 <= payload.size()) {
                pes.pts90k = static_cast<std::uint64_t>(((payload[cursor] >> 1U) & 0x07U)) << 30U;
                pes.pts90k |= static_cast<std::uint64_t>(payload[cursor + 1]) << 22U;
                pes.pts90k |= static_cast<std::uint64_t>((payload[cursor + 2] >> 1U) & 0x7FU) << 15U;
                pes.pts90k |= static_cast<std::uint64_t>(payload[cursor + 3]) << 7U;
                pes.pts90k |= static_cast<std::uint64_t>((payload[cursor + 4] >> 1U) & 0x7FU);
            }
            cursor = 9 + header_len;
            if (cursor < payload.size()) {
                pes.data.insert(pes.data.end(), payload.begin() + static_cast<std::ptrdiff_t>(cursor), payload.end());
            }
            return;
        }

        if (!pes.active) {
            return;
        }
        pes.data.insert(pes.data.end(), payload.begin(), payload.end());
    }

    void flush_pes(PesBuffer& pes, const std::function<void(EsSample&&)>& sample_sink) {
        if (!pes.active || pes.data.empty()) {
            return;
        }
        const std::uint8_t stream_type = pes.is_video ? video_stream_type_ : audio_stream_type_;
        if (pes.is_video) {
            EsSample sample;
            sample.is_video = true;
            sample.pts90k = pes.pts90k;
            sample.stream_type = stream_type;
            sample.payload = std::move(pes.data);
            const VideoCodec codec = detect_video_codec_from_stream_type(stream_type);
            if (codec == VideoCodec::kHevc) {
                sample.keyframe = hevc_annexb_has_irap(sample.payload);
            } else {
                sample.keyframe = h264_annexb_has_idr(sample.payload);
            }
            sample_sink(std::move(sample));
        } else {
            // Split ADTS audio into individual AAC frames.
            // Each frame becomes its own EsSample with interpolated PTS.
            const bool is_adts = (stream_type == 0x0F || stream_type == 0x11) &&
                                 pes.data.size() >= 7 &&
                                 pes.data[0] == 0xFF &&
                                 (pes.data[1] & 0xF0U) == 0xF0U;
            if (is_adts) {
                std::size_t offset = 0;
                std::size_t frame_index = 0;
                // Detect sample rate from first ADTS header if not yet known
                if (audio_sample_rate_ == 0) {
                    auto [rate, channels] = parse_adts_audio_params(
                        std::span<const std::uint8_t>(pes.data.data(), std::min(pes.data.size(), std::size_t{9})));
                    if (rate > 0) {
                        audio_sample_rate_ = rate;
                    }
                }
                // AAC frame PTS: use rational per-frame calculation to avoid
                // truncation drift (important for 44.1kHz where 1024*90000/44100
                // is not an integer).
                const std::uint64_t aac_sample_rate =
                    audio_sample_rate_ > 0 ? static_cast<std::uint64_t>(audio_sample_rate_) : 48000ULL;
                while (offset + 7 <= pes.data.size()) {
                    if (pes.data[offset] != 0xFF || (pes.data[offset + 1] & 0xF0U) != 0xF0U) {
                        break;
                    }
                    const bool protection_absent = (pes.data[offset + 1] & 0x01U) != 0;
                    const std::size_t header_len = protection_absent ? 7 : 9;
                    if (offset + header_len > pes.data.size()) {
                        break;
                    }
                    const std::uint16_t frame_length =
                        static_cast<std::uint16_t>(((pes.data[offset + 3] & 0x03U) << 11U) |
                                                   (pes.data[offset + 4] << 3U) |
                                                   ((pes.data[offset + 5] >> 5U) & 0x07U));
                    if (frame_length < header_len || offset + frame_length > pes.data.size()) {
                        break;
                    }
                    EsSample sample;
                    sample.is_video = false;
                    // Rational PTS: (frame_index * 1024 * 90000 + rate/2) / rate
                    sample.pts90k = pes.pts90k +
                        (frame_index * 1024ULL * 90000ULL + aac_sample_rate / 2) / aac_sample_rate;
                    sample.stream_type = stream_type;
                    sample.keyframe = false;
                    // Preserve ADTS header on first frame for codec discovery
                    if (frame_index == 0) {
                        const std::size_t copy_len = std::min(header_len, std::size_t{9});
                        std::copy_n(pes.data.begin() + static_cast<std::ptrdiff_t>(offset),
                                    copy_len, sample.adts_header.begin());
                        sample.adts_header_len = static_cast<std::uint8_t>(copy_len);
                    }
                    // Store raw AAC frame data (without ADTS header)
                    const std::size_t aac_len = frame_length - header_len;
                    sample.payload.assign(
                        pes.data.begin() + static_cast<std::ptrdiff_t>(offset + header_len),
                        pes.data.begin() + static_cast<std::ptrdiff_t>(offset + header_len + aac_len));
                    sample_sink(std::move(sample));
                    offset += frame_length;
                    ++frame_index;
                }
                // If we couldn't parse any ADTS frames, emit raw
                if (frame_index == 0) {
                    EsSample sample;
                    sample.is_video = false;
                    sample.pts90k = pes.pts90k;
                    sample.stream_type = stream_type;
                    sample.keyframe = false;
                    sample.payload = std::move(pes.data);
                    sample_sink(std::move(sample));
                }
            } else {
                // Non-ADTS audio: emit as single sample
                EsSample sample;
                sample.is_video = false;
                sample.pts90k = pes.pts90k;
                sample.stream_type = stream_type;
                sample.keyframe = false;
                sample.payload = std::move(pes.data);
                sample_sink(std::move(sample));
            }
        }
        pes = PesBuffer{};
    }

    LiveSrtCallerRuntimeConfig config_;
    std::vector<std::uint8_t> buffer_;
    std::optional<std::uint16_t> pmt_pid_;
    std::optional<std::uint16_t> video_pid_;
    std::optional<std::uint16_t> audio_pid_;
    std::uint8_t video_stream_type_ = 0;
    std::uint8_t audio_stream_type_ = 0;
    std::uint32_t audio_sample_rate_ = 0;
    bool pmt_parsed_ = false;
    std::map<std::uint16_t, PesBuffer> pes_by_pid_;

public:
    bool pmt_parsed() const { return pmt_parsed_; }
    bool has_audio() const { return audio_pid_.has_value(); }
    void set_audio_sample_rate(std::uint32_t rate) { audio_sample_rate_ = rate; }
};

std::uint64_t to_us_from_90k(std::uint64_t pts90k) {
    return (pts90k * 1000000ULL) / 90000ULL;
}

MediaFragment build_fragment_from_sample(const LiveSrtCallerRuntimeConfig& config,
                                         CallerTrackState& state,
                                         EsSample&& sample) {
    const std::string track_name = sample.is_video ? state.video_track_name : state.audio_track_name;
    const std::uint32_t track_id = sample.is_video ? state.video_track_id : state.audio_track_id;

    if (sample.is_video && config.fragment_on_keyframe && sample.keyframe) {
        if (state.first_video_keyframe_seen) {
            ++state.group_id;
        }
        state.first_video_keyframe_seen = true;
        state.object_id_by_track.clear();
    }

    if (config.fragment_on_keyframe && !state.first_video_keyframe_seen) {
        return MediaFragment{};
    }

    const std::uint64_t pts_us = to_us_from_90k(sample.pts90k);
    const std::uint64_t last_pts = state.last_pts_by_track[track_name];
    std::uint64_t duration_us = 0;
    if (last_pts != 0 && pts_us > last_pts) {
        duration_us = pts_us - last_pts;
        // Clamp unreasonably large durations (>500ms) — likely a PTS discontinuity
        if (duration_us > 500000) {
            duration_us = state.last_duration_us_by_track[track_name];
        }
    }
    if (duration_us == 0) {
        // Use last known good duration, or estimate from frame rate
        duration_us = state.last_duration_us_by_track[track_name];
        if (duration_us == 0) {
            duration_us = sample.is_video ? 33333 : ((1024ULL * 1000000ULL) / (state.audio_timescale > 0 ? state.audio_timescale : 48000U));
        }
    }
    state.last_pts_by_track[track_name] = pts_us;
    state.last_duration_us_by_track[track_name] = duration_us;

    if (sample.is_video && state.video_codec == VideoCodec::kUnknown) {
        state.video_codec = detect_video_codec_from_stream_type(sample.stream_type);
        if (state.video_codec == VideoCodec::kUnknown) {
            state.video_codec = detect_video_codec_from_annexb(sample.payload);
        }
        if (state.video_codec == VideoCodec::kUnknown) {
            state.video_codec = VideoCodec::kH264;
        }
    }

    std::vector<std::uint8_t> sample_bytes;
    if (sample.is_video) {
        sample_bytes = annexb_to_avcc(sample.payload);
    } else {
        // Audio payload is already raw AAC (ADTS stripped in flush_pes)
        sample_bytes = std::move(sample.payload);
    }

    // Accumulation-based decode_time: monotonically increasing, never resets.
    // MSE requires timestamps that never go backwards. PTS-based computation
    // can produce non-monotonic values due to SRT jitter, causing the player
    // to drop "overlapping" payloads.
    const std::uint32_t timescale = sample.is_video ? state.video_timescale : state.audio_timescale;

    // AAC audio: always use exactly 1024 samples per frame in the audio timescale.
    // Deriving from duration_us can produce 1023 or 1025 due to rounding.
    const std::uint32_t sample_duration = !sample.is_video
        ? 1024U
        : static_cast<std::uint32_t>((duration_us * static_cast<std::uint64_t>(timescale)) / 1000000ULL);

    const std::uint64_t base_decode_time = state.decode_time_by_track[track_name];
    state.decode_time_by_track[track_name] += (sample_duration == 0 ? 1U : sample_duration);
    const std::uint32_t sample_flags = (sample.is_video && !sample.keyframe) ? 0x00010000U : 0x02000000U;

    auto moof = build_moof_box(state.moof_sequence++,
                               track_id,
                               base_decode_time,
                               sample_duration == 0 ? 1 : sample_duration,
                               static_cast<std::uint32_t>(sample_bytes.size()),
                               sample_flags,
                               0,
                               config.default_base_moof);
    auto mdat = build_mdat_box(sample_bytes);

    MediaFragment fragment;
    fragment.group_id = state.group_id;
    fragment.object_id = state.object_id_by_track[track_name]++;
    fragment.track_name = track_name;
    fragment.start_time_us = pts_us;
    fragment.duration_us = duration_us;
    fragment.earliest_presentation_time_us = pts_us;
    fragment.sap_type = sample.is_video ? (sample.keyframe ? 1 : 2) : 1;
    fragment.is_video_keyframe = sample.is_video && sample.keyframe;
    fragment.creation_time_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    fragment.payload.owned_bytes.reserve(moof.size() + mdat.size());
    fragment.payload.owned_bytes.insert(fragment.payload.owned_bytes.end(), moof.begin(), moof.end());
    fragment.payload.owned_bytes.insert(fragment.payload.owned_bytes.end(), mdat.begin(), mdat.end());
    fragment.payload.span = ByteSpan{.offset = 0, .size = fragment.payload.owned_bytes.size()};
    return fragment;
}

TrackDescription make_track(std::uint32_t track_id,
                            std::string track_name,
                            bool is_video,
                            VideoCodec video_codec = VideoCodec::kH264) {
    TrackDescription track;
    track.track_id = track_id;
    track.handler_type = is_video ? "vide" : "soun";
    if (is_video) {
        if (video_codec == VideoCodec::kHevc) {
            track.codec = "hvc1.1.6.L120.B0";
            track.sample_entry_type = "hvc1";
        } else {
            track.codec = "avc1.42E01E";
            track.sample_entry_type = "avc1";
        }
    } else {
        track.codec = "mp4a.40.2";
        track.sample_entry_type = "mp4a";
    }
    track.track_name = std::move(track_name);
    track.packaging = "cmaf";
    track.mime_type = is_video ? "video/mp4" : "audio/mp4";
    track.timescale = is_video ? 90000 : 48000;
    track.width = 0;
    track.height = 0;
    track.channel_count = is_video ? 0 : 2;
    track.sample_rate = is_video ? 0 : 48000;
    track.frame_rate = is_video ? 30.0 : 0.0;
    return track;
}

std::pair<std::string, std::uint16_t> split_host_port(std::string_view endpoint) {
    const auto pos = endpoint.rfind(':');
    if (pos == std::string_view::npos || pos == 0 || pos + 1 >= endpoint.size()) {
        throw std::runtime_error("invalid SRT endpoint, expected host:port");
    }
    const std::string host(endpoint.substr(0, pos));
    const int port = std::stoi(std::string(endpoint.substr(pos + 1)));
    if (port <= 0 || port > 65535) {
        throw std::runtime_error("invalid SRT endpoint port");
    }
    return {host, static_cast<std::uint16_t>(port)};
}

}  // namespace

LiveSrtIngestManager::LiveSrtIngestManager(std::vector<LiveSrtCallerRuntimeConfig> callers,
                                           FragmentSink sink,
                                           std::atomic<bool>& stop_requested)
    : callers_(std::move(callers)),
      sink_(std::move(sink)),
      stop_requested_(stop_requested) {
    std::uint32_t next_track_id = 1000;
    for (const auto& caller : callers_) {
        bootstrap_.tracks.push_back(make_track(next_track_id++, caller.id + "_video", true, VideoCodec::kH264));
        bootstrap_.tracks.push_back(make_track(next_track_id++, caller.id + "_audio", false));
    }
    bootstrap_.init_segment = build_init_segment_from_tracks(bootstrap_.tracks);
    std::cout << "[SRT] Ingest manager created with " << callers_.size() << " caller(s)\n";
}

transport::TransportStatus LiveSrtIngestManager::start() {
#if !defined(OPENMOQ_HAS_SRT)
    return transport::TransportStatus::failure("SRT ingest requested but this build does not include libsrt");
#else
    if (callers_.empty()) {
        return transport::TransportStatus::failure("SRT ingest requested with no callers configured");
    }

    if (srt_startup() != 0) {
        return transport::TransportStatus::failure("srt_startup failed");
    }

    struct CodecDiscovery {
        std::mutex mutex;
        std::condition_variable cv;
        std::size_t discovered = 0;
        std::vector<bool> ready;
        std::vector<bool> video_private_ready;
        std::vector<bool> audio_private_ready;
        std::atomic<bool> phase_done{false};

        explicit CodecDiscovery(std::size_t count)
            : ready(count, false), video_private_ready(count, false), audio_private_ready(count, false) {}

        bool all_codec_private_ready() const {
            for (std::size_t j = 0; j < video_private_ready.size(); ++j) {
                if (!video_private_ready[j] || !audio_private_ready[j]) return false;
            }
            return true;
        }
    };
    auto discovery = std::make_shared<CodecDiscovery>(callers_.size());

    for (std::size_t i = 0; i < callers_.size(); ++i) {
        const auto caller = callers_[i];
        const auto video_track = bootstrap_.tracks[i * 2];
        const auto audio_track = bootstrap_.tracks[i * 2 + 1];
        worker_threads_.emplace_back([this,
                                      i,
                                      caller,
                                      video_track,
                                      audio_track,
                                      discovery]() {
            try {
                auto [host, port] = split_host_port(caller.endpoint);
                CallerTrackState state;
                state.video_track_name = video_track.track_name;
                state.audio_track_name = audio_track.track_name;
                state.video_track_id = video_track.track_id;
                state.audio_track_id = audio_track.track_id;
                state.video_timescale = video_track.timescale == 0 ? 90000 : video_track.timescale;
                state.audio_timescale = audio_track.timescale == 0 ? 48000 : audio_track.timescale;

                SRTSOCKET sock = srt_create_socket();
                if (sock == SRT_INVALID_SOCK) {
                    return;
                }

                const int latency = static_cast<int>(caller.latency_ms);
                srt_setsockopt(sock, 0, SRTO_LATENCY, &latency, sizeof(latency));

                // Set receive timeout so srt_recv returns periodically, allowing
                // the thread to check stop_requested_ and exit cleanly.
                const int rcv_timeout_ms = 200;
                srt_setsockopt(sock, 0, SRTO_RCVTIMEO, &rcv_timeout_ms, sizeof(rcv_timeout_ms));

                struct addrinfo hints {};
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_DGRAM;
                struct addrinfo* result = nullptr;
                if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0 || result == nullptr) {
                    srt_close(sock);
                    return;
                }

                const int connect_rc = srt_connect(sock, result->ai_addr, static_cast<int>(result->ai_addrlen));
                freeaddrinfo(result);
                if (connect_rc == SRT_ERROR) {
                    std::cerr << "[SRT] Connection FAILED to " << caller.endpoint << "\n";
                    srt_close(sock);
                    return;
                }
                std::cout << "[SRT] Connected to " << caller.endpoint
                          << " (latency=" << caller.latency_ms << "ms)\n";

                TsPesDemuxer demuxer(caller);
                std::array<std::uint8_t, 1316> recv_buf{};
                bool video_codec_private_captured = false;
                bool audio_codec_private_captured = false;
                while (!stop_requested_.load()) {
                    const int received = srt_recv(sock, reinterpret_cast<char*>(recv_buf.data()), static_cast<int>(recv_buf.size()));
                    if (received <= 0) {
                        // On any recv failure, check socket state to distinguish
                        // a transient timeout from a dead connection.
                        const SRT_SOCKSTATUS sock_state = srt_getsockstate(sock);
                        if (sock_state == SRTS_BROKEN || sock_state == SRTS_CLOSED ||
                            sock_state == SRTS_NONEXIST) {
                            std::cerr << "[SRT] Connection lost to " << caller.endpoint << "\n";
                            break;
                        }
                        // Timeout or transient error — loop back to check stop flag.
                        continue;
                    }
                    demuxer.feed(recv_buf.data(), static_cast<std::size_t>(received),
                                [&](EsSample&& sample) {
                                    if (sample.is_video && state.video_codec == VideoCodec::kUnknown) {
                                        state.video_codec = detect_video_codec_from_stream_type(sample.stream_type);
                                        if (state.video_codec == VideoCodec::kUnknown) {
                                            state.video_codec = detect_video_codec_from_annexb(sample.payload);
                                        }
                                        if (state.video_codec != VideoCodec::kUnknown &&
                                            !discovery->phase_done.load(std::memory_order_acquire)) {
                                            std::lock_guard<std::mutex> lock(discovery->mutex);
                                            if (discovery->phase_done.load(std::memory_order_relaxed)) {
                                                // Main thread finalized; skip track mutation.
                                            } else if (!discovery->ready[i]) {
                                                discovery->ready[i] = true;
                                                ++discovery->discovered;
                                                const std::size_t video_index = i * 2;
                                                if (video_index < bootstrap_.tracks.size()) {
                                                    bootstrap_.tracks[video_index] = make_track(
                                                        bootstrap_.tracks[video_index].track_id,
                                                        bootstrap_.tracks[video_index].track_name,
                                                        true,
                                                        state.video_codec);
                                                }
                                            }
                                            discovery->cv.notify_one();
                                        }
                                    }
                                    // Extract video codec_private (avcC/hvcC) from first keyframe
                                    if (sample.is_video && !video_codec_private_captured &&
                                        sample.keyframe &&
                                        !discovery->phase_done.load(std::memory_order_acquire)) {
                                        std::vector<std::uint8_t> codec_priv;
                                        std::string codec_str_update;
                                        std::pair<std::uint32_t, std::uint32_t> dim_update{0, 0};
                                        if (state.video_codec == VideoCodec::kHevc) {
                                            auto params = extract_hevc_param_sets(sample.payload);
                                            codec_priv = build_hvcc_box(params);
                                            dim_update = parse_hevc_dimensions(params.sps);
                                            if (!codec_priv.empty() && params.sps.size() > 12) {
                                                // Build codec string: hvc1.<profile>.<compat>.<tier><level>
                                                const std::uint8_t* s = params.sps.data() + 2;
                                                const std::uint8_t prof_idc = static_cast<std::uint8_t>(s[1] & 0x1FU);
                                                const std::uint8_t tier_flag = static_cast<std::uint8_t>((s[1] >> 5U) & 0x01U);
                                                const std::uint32_t compat = (static_cast<std::uint32_t>(s[2]) << 24U) |
                                                                              (static_cast<std::uint32_t>(s[3]) << 16U) |
                                                                              (static_cast<std::uint32_t>(s[4]) << 8U) |
                                                                              static_cast<std::uint32_t>(s[5]);
                                                const std::uint8_t level_idc = s[12];
                                                char buf[64];
                                                std::snprintf(buf, sizeof(buf), "hvc1.%u.%X.%c%u",
                                                              prof_idc, compat, tier_flag ? 'H' : 'L', level_idc / 3);
                                                codec_str_update = buf;
                                            }
                                        } else {
                                            auto [sps, pps] = extract_h264_sps_pps(sample.payload);
                                            codec_priv = build_avcc_box(sps, pps);
                                            dim_update = parse_h264_dimensions(sps);
                                            if (!codec_priv.empty() && sps.size() >= 4) {
                                                char buf[32];
                                                std::snprintf(buf, sizeof(buf), "avc1.%02X%02X%02X",
                                                              sps[1], sps[2], sps[3]);
                                                codec_str_update = buf;
                                            }
                                        }
                                        if (!codec_priv.empty()) {
                                            video_codec_private_captured = true;
                                            std::lock_guard<std::mutex> lock(discovery->mutex);
                                            if (!discovery->phase_done.load(std::memory_order_relaxed)) {
                                                const std::size_t video_index = i * 2;
                                                if (video_index < bootstrap_.tracks.size()) {
                                                    bootstrap_.tracks[video_index].codec_private = std::move(codec_priv);
                                                    if (!codec_str_update.empty()) {
                                                        bootstrap_.tracks[video_index].codec = codec_str_update;
                                                    }
                                                    if (dim_update.first > 0 && dim_update.second > 0) {
                                                        bootstrap_.tracks[video_index].width = dim_update.first;
                                                        bootstrap_.tracks[video_index].height = dim_update.second;
                                                    }
                                                    discovery->video_private_ready[i] = true;
                                                    std::cout << "[SRT] Video "
                                                              << (state.video_codec == VideoCodec::kHevc ? "hvcC" : "avcC")
                                                              << " extracted ("
                                                              << bootstrap_.tracks[video_index].codec_private.size()
                                                              << " bytes) codec=" << bootstrap_.tracks[video_index].codec
                                                              << " from " << caller.id << "\n";
                                                }
                                            }
                                            discovery->cv.notify_one();
                                        }
                                    }
                                    // Extract audio codec_private (esds) from first ADTS frame
                                    if (!sample.is_video && !audio_codec_private_captured &&
                                        !discovery->phase_done.load(std::memory_order_acquire) &&
                                        sample.adts_header_len >= 7) {
                                        auto codec_priv = build_esds_box(
                                            std::span<const std::uint8_t>(sample.adts_header.data(), sample.adts_header_len));
                                        if (!codec_priv.empty()) {
                                            audio_codec_private_captured = true;
                                            // Parse actual sample_rate and channel_count from ADTS header
                                            auto [adts_rate, adts_channels] = parse_adts_audio_params(
                                                std::span<const std::uint8_t>(sample.adts_header.data(), sample.adts_header_len));
                                            if (adts_rate > 0) {
                                                state.audio_timescale = adts_rate;
                                                demuxer.set_audio_sample_rate(adts_rate);
                                            }
                                            std::lock_guard<std::mutex> lock(discovery->mutex);
                                            if (!discovery->phase_done.load(std::memory_order_relaxed)) {
                                                const std::size_t audio_index = i * 2 + 1;
                                                if (audio_index < bootstrap_.tracks.size()) {
                                                    bootstrap_.tracks[audio_index].codec_private = std::move(codec_priv);
                                                    if (adts_rate > 0) {
                                                        bootstrap_.tracks[audio_index].sample_rate = adts_rate;
                                                        bootstrap_.tracks[audio_index].timescale = adts_rate;
                                                    }
                                                    if (adts_channels > 0) {
                                                        bootstrap_.tracks[audio_index].channel_count = adts_channels;
                                                    }
                                                    discovery->audio_private_ready[i] = true;
                                                    std::cout << "[SRT] Audio esds extracted ("
                                                              << bootstrap_.tracks[audio_index].codec_private.size()
                                                              << " bytes) from " << caller.id << "\n";
                                                }
                                            }
                                            discovery->cv.notify_one();
                                        }
                                    }
                                    auto fragment = build_fragment_from_sample(caller, state, std::move(sample));
                                    if (fragment.payload.owned_bytes.empty()) {
                                        return;
                                    }
                                    sink_(std::move(fragment));
                                });
                    // After feed: if PMT parsed and no audio PID found, mark audio discovery done early
                    if (!audio_codec_private_captured && demuxer.pmt_parsed() && !demuxer.has_audio() &&
                        !discovery->phase_done.load(std::memory_order_acquire)) {
                        audio_codec_private_captured = true;
                        std::lock_guard<std::mutex> lock(discovery->mutex);
                        if (!discovery->phase_done.load(std::memory_order_relaxed)) {
                            discovery->audio_private_ready[i] = true;
                            std::cout << "[SRT] No audio PID in PMT for " << caller.id << "\n";
                        }
                        discovery->cv.notify_one();
                    }
                }
                if (!video_codec_private_captured) {
                    std::cerr << "[SRT] Worker exited without discovering video codec_private for "
                              << caller.id << "\n";
                }
                srt_close(sock);
            } catch (...) {
            }
        });
    }

    {
        std::unique_lock<std::mutex> lock(discovery->mutex);
        discovery->cv.wait_for(lock,
                              std::chrono::seconds(5),
                              [&]() {
                                  return discovery->discovered >= callers_.size() &&
                                         discovery->all_codec_private_ready();
                              });
        // Set phase_done while holding the mutex so workers that already passed
        // the phase_done check but haven't acquired the mutex yet will re-check.
        discovery->phase_done.store(true, std::memory_order_release);

        // Remove tracks that have no codec_private (e.g. audio track when feed has no audio).
        // This ensures the catalog only advertises tracks that are actually present in the feed.
        bootstrap_.tracks.erase(
            std::remove_if(bootstrap_.tracks.begin(), bootstrap_.tracks.end(),
                           [](const TrackDescription& t) {
                               return t.codec_private.empty();
                           }),
            bootstrap_.tracks.end());

        bootstrap_.init_segment = build_init_segment_from_tracks(bootstrap_.tracks);
    }
    std::cout << "[SRT] Codec discovery complete. Init segment rebuilt ("
              << bootstrap_.init_segment.size() << " bytes)\n";
    for (const auto& track : bootstrap_.tracks) {
        std::cout << "[SRT]   Track: " << track.track_name
                  << " codec=" << track.codec
                  << " codec_private=" << track.codec_private.size() << " bytes\n";
    }

    return transport::TransportStatus::success();
#endif
}

void LiveSrtIngestManager::join() {
    for (auto& worker : worker_threads_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
#if defined(OPENMOQ_HAS_SRT)
    srt_cleanup();
#endif
}

const LiveSrtBootstrap& LiveSrtIngestManager::bootstrap() const {
    return bootstrap_;
}

std::vector<std::uint8_t> LiveSrtIngestManager::build_synthetic_init_segment(const std::vector<TrackDescription>& tracks) {
    return build_init_segment_from_tracks(tracks);
}

}  // namespace openmoq::publisher
