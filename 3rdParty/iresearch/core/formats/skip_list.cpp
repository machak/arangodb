//
// IResearch search engine 
// 
// Copyright (c) 2016 by EMC Corporation, All Rights Reserved
// 
// This software contains the intellectual property of EMC Corporation or is licensed to
// EMC Corporation from third parties. Use of this software and the intellectual property
// contained therein is expressly limited to the terms and conditions of the License
// Agreement under which it is provided by or on behalf of EMC.
// 

#include "shared.hpp"
#include "skip_list.hpp"

#include "store/store_utils.hpp"

#include "index/iterators.hpp"

#include "utils/math_utils.hpp"
#include "utils/std.hpp"

NS_LOCAL

// returns maximum number of skip levels needed to store specified
// count of objects for skip list with
// step skip_0 for 0 level, step skip_n for other levels
inline size_t max_levels(size_t skip_0, size_t skip_n, size_t count) {
  size_t levels = 0;
  if (skip_0 < count) {
    levels = 1 + iresearch::math::log(count/skip_0, skip_n);
  }
  return levels;
}

const size_t UNDEFINED = iresearch::integer_traits<size_t>::const_max;

NS_END // LOCAL

NS_ROOT

// ----------------------------------------------------------------------------
// --SECTION--                                       skip_writer implementation
// ----------------------------------------------------------------------------

skip_writer::skip_writer(size_t skip_0, size_t skip_n) NOEXCEPT 
  : skip_0_(skip_0), skip_n_(skip_n) {
}

void skip_writer::prepare(
    size_t max_levels, 
    size_t count, 
    const skip_writer::write_f& write /* = nop */) {
  max_levels = std::max(size_t(1), max_levels);
  levels_.resize(std::min(max_levels, ::max_levels(skip_0_, skip_n_, count)));
  write_ = write;
}

void skip_writer::skip(size_t count) {
  assert(!levels_.empty());

  if (0 != count % skip_0_) {
    return;
  }

  uint64_t child = 0;

  // write 0 level
  {
    auto& stream = levels_.front().stream;
    write_(0, stream);
    count /= skip_0_;
    child = stream.file_pointer();
  }

  // write levels from 1 to n
  size_t num = 0;
  for (auto level = levels_.begin()+1, end = levels_.end();
       0 == count % skip_n_ && level != end;
       ++level, count /= skip_n_) {
    auto& stream = level->stream;
    write_(++num, stream);

    uint64_t next_child = stream.file_pointer();
    stream.write_vlong(child);
    child = next_child;
  }
}

void skip_writer::flush(index_output& out) {
  const auto rend = levels_.rend();

  // find first filled level
  auto level = std::find_if(
    levels_.rbegin(), rend,
    [](const memory_output& level) {
      return level.stream.file_pointer();
  });

  // write number of levels
  out.write_vint(uint32_t(std::distance(level, rend)));

  // write levels from n downto 0
  std::for_each(
    level, rend,
    [&out](memory_output& level) {
      auto& stream = level.stream;
      stream.flush(); // update length of each buffer

      const uint64_t length = stream.file_pointer();
      assert(length);
      out.write_vlong(length);
      stream >> out;
  });
}

void skip_writer::reset() {
  for(auto& level : levels_) {
    level.stream.reset();
  }
}

// ----------------------------------------------------------------------------
// --SECTION--                                       skip_reader implementation
// ----------------------------------------------------------------------------

skip_reader::level::level(
    index_input::ptr&& stream,
    size_t step,
    uint64_t begin, 
    uint64_t end) NOEXCEPT
  : stream(stream->reopen()), // thread-safe input
    begin(begin), 
    end(end),
    step(step) {
}

skip_reader::level::level(skip_reader::level&& rhs) NOEXCEPT 
  : stream(std::move(rhs.stream)),
    begin(rhs.begin), end(rhs.end),
    child(rhs.child),
    step(rhs.step), 
    skipped(rhs.skipped),
    doc(rhs.doc) {
}

skip_reader::level::level(const skip_reader::level& rhs)
  : stream(rhs.stream->dup()), // dup of a reopen()ed input
    begin(rhs.begin), end(rhs.end),
    child(rhs.child),
    step(rhs.step), 
    skipped(rhs.skipped),
    doc(rhs.doc) {
}

index_input::ptr skip_reader::level::dup() const NOEXCEPT {
  try {
    return index_input::make<skip_reader::level>(*this);
  } catch(...) {
    IR_EXCEPTION();
  }

  return nullptr;
}

byte_type skip_reader::level::read_byte() {
  return stream->read_byte();
}

size_t skip_reader::level::read_bytes(byte_type* b, size_t count) {
  static_assert(sizeof(size_t) >= sizeof(uint64_t), "sizeof(size_t) < sizeof(uint64_t)");
  return stream->read_bytes(b, std::min(size_t(end) - file_pointer(), count));
}

index_input::ptr skip_reader::level::reopen() const NOEXCEPT {
  level tmp(*this);

  tmp.stream = tmp.stream->reopen();

  return index_input::make<skip_reader::level>(std::move(tmp));
}

size_t skip_reader::level::file_pointer() const {
  return stream->file_pointer() - begin;
}

size_t skip_reader::level::length() const {
  return end - begin;
}

bool skip_reader::level::eof() const {
  return stream->file_pointer() >= end;
}

void skip_reader::level::seek(size_t pos) {
  return stream->seek(begin + pos);
}

skip_reader::skip_reader(
    size_t skip_0, 
    size_t skip_n) NOEXCEPT
  : skip_0_(skip_0), skip_n_(skip_n) {
}

void skip_reader::read_skip(skip_reader::level& level) {
  // read_ should return NO_MORE_DOCS when stream is exhausted
  const auto doc = read_(size_t(std::distance(&level, &levels_.back())), level);

  // read pointer to child level if needed
  if (!type_limits<type_t::doc_id_t>::eof(doc) && level.child != UNDEFINED) {
    level.child = level.stream->read_vlong();
  }

  level.doc = doc;
  level.skipped += level.step;
}

/* static */ void skip_reader::seek_skip(
    skip_reader::level& level, 
    uint64_t ptr,
    size_t skipped) {
  auto &stream = *level.stream;
  const auto absolute_ptr = level.begin + ptr;
  if (absolute_ptr > stream.file_pointer()) {
    stream.seek(absolute_ptr);
    level.skipped = skipped;
    if (level.child != UNDEFINED) {
      level.child = stream.read_vlong();
    }
  }
}

// returns highest level with the value not less than target 
skip_reader::levels_t::iterator skip_reader::find_level(doc_id_t target) {
  assert(std::is_sorted(
    levels_.rbegin(), levels_.rend(),
    [](level& lhs, level& rhs) { return lhs.doc < rhs.doc; }
  ));

  auto level = std::upper_bound(
    levels_.rbegin(),
    levels_.rend(),
    target,
    [](doc_id_t target, const skip_reader::level& level) {
      return target < level.doc;
  });

  if (level == levels_.rend()) {
    return levels_.begin(); // the highest
  }

  // check if we have already found the lowest possible level
  if (level != levels_.rbegin()) {
    --level;
  }

  // convert reverse iterator to forward
  return irstd::to_forward(level);
}

size_t skip_reader::seek(doc_id_t target) {
  assert(!levels_.empty());

  auto level = find_level(target); // the highest level for the specified target
  uint64_t child = 0; // pointer to child skip
  size_t skipped = 0; // number of skipped documents

  std::for_each(
    level, levels_.end(),
    [this, &child, &skipped, &target](skip_reader::level& level) {
      if (level.doc < target) {
        // seek to child
        seek_skip(level, child, skipped);

        // seek to skip
        child = level.child;
        read_skip(level);

        for (; level.doc < target; read_skip(level)) {
          child = level.child;
        }

        skipped = level.skipped - level.step;
      }
  });

  skipped = levels_.back().skipped;
  return skipped ? skipped - skip_0_ : 0;
}

void skip_reader::reset() {
  static auto reset = [](skip_reader::level& level) {
    level.stream->seek(level.begin);
    if (level.child != UNDEFINED) {
      level.child = 0;
    }
    level.skipped = 0;
    level.doc = type_limits<type_t::doc_id_t>::invalid();
  };

  std::for_each(levels_.begin(), levels_.end(), reset);
}

void skip_reader::load_level(levels_t& levels, index_input::ptr&& stream, size_t step) {
  // read level length
  const auto length = stream->read_vlong();
  if (!length) {
    // corrupted index
    throw index_error();
  }
  const auto begin = stream->file_pointer();
  const auto end = begin + length;

  // load level
  levels.emplace_back(std::move(stream), step, begin, end);
}

void skip_reader::prepare(index_input::ptr&& in, const read_f& read /* = nop */) {
  // read number of levels in a skip-list
  size_t max_levels = in->read_vint();

  if (max_levels) {
    std::vector<level> levels;
    levels.reserve(max_levels);

    size_t step = skip_0_ * size_t(pow(skip_n_, --max_levels)); // skip step of the level

    // load levels from n down to 1
    for (; max_levels; --max_levels) {
      load_level(levels, in->dup(), step);

      // seek to the next level
      in->seek(levels.back().end);

      step /= skip_n_;
    }

    // load 0 level
    load_level(levels, std::move(in), skip_0_);
    levels.back().child = UNDEFINED;

    // noexcept
    levels_ = std::move(levels);
  }

  // noexcept
  read_ = read;
}

NS_END