#include "openmoq/publisher/locmaf_encoder.h"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <string_view>

namespace openmoq::publisher {
namespace {
using Bytes = std::vector<uint8_t>;
using Span = std::span<const uint8_t>;
using Values = std::vector<int64_t>;
using Fields = std::map<unsigned, Values>;
[[noreturn]] void bad(const char *text) { throw std::runtime_error(text); }
struct Outside {};
struct Reader {
  Span b;
  size_t p = 0;
  uint64_t get(size_t n) {
    if (n > b.size() - p)
      bad("LOCMAF: truncated BMFF field");
    uint64_t v = 0;
    while (n--)
      v = (v << 8) | b[p++];
    return v;
  }
  Span take(size_t n) {
    if (n > b.size() - p)
      bad("LOCMAF: truncated BMFF bytes");
    auto out = b.subspan(p, n);
    p += n;
    return out;
  }
  void end() const {
    if (p != b.size())
      bad("LOCMAF: unexpected BMFF trailing fields");
  }
};
struct Box {
  std::string_view type;
  Span all;
  Span data;
};
std::vector<Box> boxes(Span data) {
  Reader r{data};
  std::vector<Box> result;
  while (r.p < data.size()) {
    const auto start = r.p;
    const auto n = r.get(4);
    const auto name = r.take(4);
    if (n < 8 || n > data.size() - start)
      bad("LOCMAF: invalid BMFF box size (size escapes unsupported)");
    const auto payload = r.take(static_cast<size_t>(n) - 8);
    result.push_back({{reinterpret_cast<const char *>(name.data()), 4},
                      data.subspan(start, n),
                      payload});
  }
  return result;
}
Box one(const std::vector<Box> &list, std::string_view type) {
  const Box *found = nullptr;
  for (const auto &b : list)
    if (b.type == type) {
      if (found)
        bad("LOCMAF: duplicate required box");
      found = &b;
    }
  if (!found)
    bad("LOCMAF: missing required box");
  return *found;
}
void excluded(const std::vector<Box> &list, bool fragment, unsigned depth = 0) {
  if (depth > 32)
    bad("LOCMAF: excessive BMFF nesting");
  for (const auto &b : list) {
    if (b.type == "sgpd" || b.type == "sbgp" || b.type == "subs" ||
        (fragment && b.type == "pssh"))
      throw LocmafIneligible("LOCMAF: track requires excluded sample groups, "
                             "subs, or fragment pssh");
    if (b.type == "moov" || b.type == "trak" || b.type == "mdia" ||
        b.type == "minf" || b.type == "stbl" || b.type == "moof" ||
        b.type == "traf")
      excluded(boxes(b.data), fragment, depth + 1);
  }
}
void vi(Bytes &b, uint64_t value) {
  unsigned n = 1;
  while (n < 9 && value >= (uint64_t{1} << (7 * n)))
    ++n;
  const auto start = b.size();
  b.resize(start + n);
  for (unsigned i = n; i > 0; --i) {
    b[start + i - 1] = static_cast<uint8_t>(value);
    value >>= 8;
  }
  if (n > 1)
    b[start] |= static_cast<uint8_t>(0xffu << (9 - n));
}
uint64_t zz(int64_t v) {
  return v >= 0 ? uint64_t(v) * 2 : uint64_t(-(v + 1)) * 2 + 1;
}
bool equal(const Values &v, size_t from = 0) {
  return v.empty() || std::all_of(v.begin() + from, v.end(),
                                  [&](auto x) { return x == v[from]; });
}
struct Context {
  uint32_t track, description, duration, size, flags;
  bool protected_track = false;
  unsigned iv = 0;
};
Context context(Span init) {
  const auto top = boxes(init);
  excluded(top, false);
  const auto moov = boxes(one(top, "moov").data);
  if (std::count_if(moov.begin(), moov.end(),
                    [](auto b) { return b.type == "trak"; }) != 1)
    throw LocmafIneligible(
        "LOCMAF: initialization must contain exactly one track");
  const auto trak = boxes(one(moov, "trak").data);
  Reader tk{one(trak, "tkhd").data};
  const auto tv = tk.get(4) >> 24;
  if (tv > 1)
    bad("LOCMAF: unsupported tkhd version");
  tk.take(tv ? 16 : 8);
  Context c{};
  c.track = tk.get(4);
  const auto mdia = boxes(one(trak, "mdia").data);
  Reader md{one(mdia, "mdhd").data};
  const auto mv = md.get(4) >> 24;
  if (mv > 1)
    bad("LOCMAF: unsupported mdhd version");
  md.take(mv ? 16 : 8);
  if (!md.get(4))
    bad("LOCMAF: zero timescale");
  const auto mvex = boxes(one(moov, "mvex").data);
  bool matched = false;
  for (const auto &b : mvex)
    if (b.type == "trex") {
      Reader r{b.data};
      if (r.get(4))
        bad("LOCMAF: invalid trex version/flags");
      const auto id = r.get(4);
      const auto desc = r.get(4), dur = r.get(4), size = r.get(4),
                 flags = r.get(4);
      r.end();
      if (id == c.track) {
        if (matched)
          bad("LOCMAF: duplicate trex");
        matched = true;
        c.description = desc;
        c.duration = dur;
        c.size = size;
        c.flags = flags;
      }
    }
  if (!matched)
    bad("LOCMAF: no matching trex");
  const auto minf = boxes(one(mdia, "minf").data);
  const auto stbl = boxes(one(minf, "stbl").data);
  Reader sd{one(stbl, "stsd").data};
  if (sd.get(4))
    bad("LOCMAF: invalid stsd");
  const auto count = sd.get(4);
  const auto entries = boxes(sd.take(sd.b.size() - sd.p));
  if (entries.size() != count)
    bad("LOCMAF: invalid stsd entry count");
  for (const auto &entry : entries) {
    if (entry.type != "encv" && entry.type != "enca")
      continue;
    if (entries.size() != 1)
      throw LocmafIneligible(
          "LOCMAF: multiple protected sample descriptions unsupported");
    Reader e{entry.data};
    size_t fixed = 78;
    if (entry.type == "enca") {
      e.take(8);
      const auto version = e.get(2);
      fixed = version == 0 ? 28 : version == 1 ? 44 : version == 2 ? 64 : 0;
    }
    if (!fixed)
      throw LocmafIneligible("LOCMAF: unsupported audio sample entry version");
    e.p = 0;
    e.take(fixed);
    const auto children = boxes(e.take(e.b.size() - e.p));
    const auto sinf = boxes(one(children, "sinf").data);
    const auto schi = boxes(one(sinf, "schi").data);
    Reader t{one(schi, "tenc").data};
    const auto vf = t.get(4);
    if ((vf >> 24) > 1 || (vf & 0xffffff))
      bad("LOCMAF: invalid tenc");
    t.take(2);
    const auto prot = t.get(1);
    if (prot > 1)
      bad("LOCMAF: invalid protection flag");
    c.protected_track = prot;
    c.iv = t.get(1);
    t.take(16);
    if (c.iv != 0 && c.iv != 8 && c.iv != 16)
      bad("LOCMAF: invalid IV size");
    if (prot && !c.iv) {
      const auto n = t.get(1);
      if (n != 8 && n != 16)
        bad("LOCMAF: invalid constant IV");
      t.take(n);
    }
    t.end();
  }
  return c;
}
struct Parsed {
  Fields fields;
  Bytes ivs;
  Span payload;
  std::vector<Box> prefix;
  uint64_t bmdt = 0, duration = 0;
};
void encryption(Parsed &out, const Box &senc, const Context &c,
                const Values &sizes) {
  if (!c.protected_track)
    throw Outside{};
  Reader r{senc.data};
  const auto vf = r.get(4);
  if (vf & ~uint64_t{2})
    throw Outside{};
  if (r.get(4) != sizes.size())
    bad("LOCMAF: senc sample count mismatch");
  Values counts, clear, encrypted;
  for (auto size : sizes) {
    auto iv = r.take(c.iv);
    out.ivs.insert(out.ivs.end(), iv.begin(), iv.end());
    if (vf & 2) {
      const auto n = r.get(2);
      counts.push_back(n);
      if (c.iv + 2 + 6 * n > 255)
        bad("LOCMAF: CENC auxiliary information exceeds saiz capacity");
      uint64_t sum = 0;
      for (uint64_t j = 0; j < n; ++j) {
        auto a = r.get(2), b = r.get(4);
        clear.push_back(a);
        encrypted.push_back(b);
        sum += a + b;
      }
      if (n && sum != uint64_t(size))
        bad("LOCMAF: subsamples do not cover sample");
    }
  }
  r.end();
  if (c.iv)
    out.fields[9] = {};
  if (vf & 2) {
    out.fields[11] = std::move(counts);
    out.fields[13] = std::move(clear);
    out.fields[15] = std::move(encrypted);
  }
}
Parsed parse(const std::vector<Box> &top, const Context &c) {
  auto it = std::find_if(top.begin(), top.end(),
                         [](auto b) { return b.type == "moof"; });
  if (it == top.end() || top.end() - it != 2 || (it + 1)->type != "mdat")
    throw Outside{};
  Parsed out;
  out.prefix.assign(top.begin(), it);
  out.payload = (it + 1)->data;
  for (auto b : out.prefix)
    if (b.type == "mdat" || b.type == "moof")
      throw Outside{};
  const auto mc = boxes(it->data);
  for (auto b : mc)
    if (b.type != "mfhd" && b.type != "traf")
      throw Outside{};
  if (std::count_if(mc.begin(), mc.end(),
                    [](auto b) { return b.type == "traf"; }) != 1)
    throw Outside{};
  Reader mf{one(mc, "mfhd").data};
  if (mf.get(4))
    throw Outside{};
  mf.get(4);
  mf.end();
  const auto tc = boxes(one(mc, "traf").data);
  for (auto b : tc)
    if (b.type != "tfhd" && b.type != "tfdt" && b.type != "trun" &&
        b.type != "senc" && b.type != "saiz" && b.type != "saio")
      throw Outside{};
  if (std::count_if(tc.begin(), tc.end(),
                    [](auto b) { return b.type == "trun"; }) != 1)
    throw Outside{};
  Reader tf{one(tc, "tfhd").data};
  auto tf_flags = tf.get(4);
  if (tf_flags & ~uint64_t{0x02003a})
    throw Outside{};
  if (tf.get(4) != c.track)
    throw LocmafIneligible("LOCMAF: fragment track differs from init");
  auto description = tf_flags & 2 ? tf.get(4) : c.description;
  auto duration = tf_flags & 8 ? tf.get(4) : c.duration;
  auto size = tf_flags & 16 ? tf.get(4) : c.size;
  auto flags = tf_flags & 32 ? tf.get(4) : c.flags;
  tf.end();
  if (description != c.description)
    out.fields[2] = {int64_t(description)};
  Reader td{one(tc, "tfdt").data};
  auto dt_flags = td.get(4);
  if (dt_flags != 0 && dt_flags != 0x01000000)
    throw Outside{};
  out.bmdt = td.get(dt_flags ? 8 : 4);
  td.end();
  Reader tr{one(tc, "trun").data};
  auto tr_flags = tr.get(4);
  const auto version = tr_flags >> 24;
  tr_flags &= 0xffffff;
  if (version > 1 || (tr_flags & ~uint64_t{0xf05}) ||
      ((tr_flags & 4) && (tr_flags & 0x400)))
    throw Outside{};
  const auto n = tr.get(4);
  if (n > 1000000)
    bad("LOCMAF: sample count exceeds resource bound");
  if (!(tr_flags & 1) || tr.get(4) != it->all.size() + 8)
    throw Outside{};
  const auto first = tr_flags & 4 ? tr.get(4) : flags;
  Values durations, sizes, sample_flags, ctos;
  uint64_t total_size = 0;
  for (uint64_t i = 0; i < n; ++i) {
    auto d = tr_flags & 0x100 ? tr.get(4) : duration;
    auto s = tr_flags & 0x200 ? tr.get(4) : size;
    auto f = tr_flags & 0x400 ? tr.get(4) : i ? flags : first;
    auto ct = tr_flags & 0x800 ? tr.get(4) : 0;
    durations.push_back(d);
    sizes.push_back(s);
    sample_flags.push_back(f);
    ctos.push_back(version && ct >= 0x80000000 ? int64_t(ct) - 0x100000000LL
                                               : int64_t(ct));
    out.duration += d;
    total_size += s;
  }
  tr.end();
  if (total_size != out.payload.size())
    bad("LOCMAF: sample sizes do not cover mdat");
  out.fields[14] = {int64_t(n)};
  if (n) {
    if (!equal(durations))
      out.fields[3] = durations;
    else if (durations[0] != c.duration)
      out.fields[4] = {durations[0]};
    if (n > 1) {
      if (!equal(sizes))
        out.fields[1] = Values(sizes.begin(), sizes.end() - 1);
      else if (sizes[0] != c.size)
        out.fields[6] = {sizes[0]};
    }
    if (equal(sample_flags)) {
      if (sample_flags[0] != c.flags)
        out.fields[8] = {sample_flags[0]};
    } else if (equal(sample_flags, 1)) {
      out.fields[12] = {sample_flags[0]};
      if (sample_flags[1] != c.flags)
        out.fields[8] = {sample_flags[1]};
    } else
      out.fields[7] = sample_flags;
    if (std::any_of(ctos.begin(), ctos.end(), [](auto v) { return v != 0; }))
      out.fields[5] = ctos;
  }
  auto senc = std::find_if(tc.begin(), tc.end(),
                           [](auto b) { return b.type == "senc"; });
  if (senc != tc.end())
    encryption(out, one(tc, "senc"), c, sizes);
  else if ((c.protected_track && c.iv && n) ||
           std::any_of(tc.begin(), tc.end(), [](const Box &b) {
             return b.type == "saiz" || b.type == "saio";
           }))
    throw Outside{};
  return out;
}
void field(Bytes &out, unsigned id, const Values &values, bool full,
           const Values *before) {
  vi(out, id);
  Bytes data;
  for (size_t i = 0; i < values.size(); ++i) {
    int64_t v = values[i];
    if (!full && id != 27)
      v -= before && i < before->size() ? (*before)[i] : 0;
    vi(data, (!full && id != 27) || id == 5 ? zz(v) : uint64_t(v));
  }
  if (id & 1)
    vi(out, data.size());
  out.insert(out.end(), data.begin(), data.end());
}
} // namespace
struct LocmafEncoder::State {
  Context context;
  Fields previous;
  Bytes ivs;
  uint64_t group = 0, object = 0, bmdt = 0, duration = 0;
  bool anchored = false;
};
LocmafEncoder::LocmafEncoder(Span init) : state_(std::make_unique<State>()) {
  state_->context = context(init);
}
LocmafEncoder::~LocmafEncoder() = default;
LocmafEncoder::LocmafEncoder(LocmafEncoder &&) noexcept = default;
LocmafEncoder &LocmafEncoder::operator=(LocmafEncoder &&) noexcept = default;
void LocmafEncoder::reset() {
  state_->anchored = false;
  state_->previous.clear();
  state_->ivs.clear();
}
Bytes LocmafEncoder::encode(Span chunk, uint64_t group, uint64_t object,
                            bool force_full) {
  const auto top = boxes(chunk);
  if (top.empty())
    bad("LOCMAF: empty chunk");
  excluded(top, true);
  Parsed p;
  try {
    p = parse(top, state_->context);
  } catch (const Outside &) {
    Bytes raw{4};
    raw.insert(raw.end(), chunk.begin(), chunk.end());
    reset();
    return raw;
  }
  const auto &s = *state_;
  const bool full = force_full || !s.anchored || group != s.group ||
                    s.object == UINT64_MAX || object != s.object + 1 ||
                    s.duration > UINT64_MAX - s.bmdt ||
                    p.bmdt != s.bmdt + s.duration;
  Bytes properties;
  auto fields = p.fields;
  if (full)
    fields[10] = {};
  else {
    Values removed;
    for (const auto &[id, value] : s.previous)
      if (!fields.contains(id))
        removed.push_back(id);
    if (!removed.empty())
      fields[27] = std::move(removed);
  }
  for (const auto &[id, values] : fields) {
    auto prev = s.previous.find(id);
    const Values *before = prev == s.previous.end() ? nullptr : &prev->second;
    if (id == 10) {
      vi(properties, 10);
      vi(properties, p.bmdt);
    } else if (id == 9) {
      if (!full && before && p.ivs == s.ivs)
        continue;
      vi(properties, 9);
      vi(properties, p.ivs.size());
      properties.insert(properties.end(), p.ivs.begin(), p.ivs.end());
    } else if (full || !before || values != *before)
      field(properties, id, values, full, before);
  }
  Bytes output;
  for (auto b : p.prefix) {
    vi(output, 1);
    vi(output, b.all.size() - 4);
    output.insert(output.end(), b.all.begin() + 4, b.all.end());
  }
  vi(output, full ? 2 : 3);
  vi(output, properties.size());
  output.insert(output.end(), properties.begin(), properties.end());
  output.insert(output.end(), p.payload.begin(), p.payload.end());
  state_->previous = std::move(p.fields);
  state_->ivs = std::move(p.ivs);
  state_->group = group;
  state_->object = object;
  state_->bmdt = p.bmdt;
  state_->duration = p.duration;
  state_->anchored = true;
  return output;
}
} // namespace openmoq::publisher
