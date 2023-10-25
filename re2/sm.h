// Copyright 2010 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef RE2_SM_H_
#define RE2_SM_H_

#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "re2/re2.h"

namespace re2 {

class Prog;
class Regexp;
class DFA;
class RWLocker;

class RE2::SM {
 public:
  class State;

  enum ExecResult {
	  kErrorInconsistent = -2, // reverse scan couldn't find a match (inconsistent data)
	  kErrorOutOfMemory  = -1, // out-of-memory
	  kMismatch          = 0,  // match can't be found; next exec will reset & restart
	  kContinue,               // match not found yet; continue feeding next chunks of data
	  kContinueBackward,       // match end found; continue feeding previous chunks of data
	  kMatch,                  // match end & start found
  };

 public:
  explicit SM(const char* pattern) {
    init(pattern, RE2::DefaultOptions);
  }
  explicit SM(const std::string& pattern) {
    init(pattern, RE2::DefaultOptions);
  }
  explicit SM(absl::string_view pattern) {
    init(pattern, RE2::DefaultOptions);
  }
  SM(absl::string_view pattern, const Options& options) {
    init(pattern, options);
  }
  ~SM();

  bool ok() const {
    return error_code_ == NoError;
  }
  ErrorCode error_code() const {
    return error_code_;
  }
  const std::string& error() const {
    return error_;
  }
  const std::string& error_arg() const {
    return error_arg_;
  }

  ExecResult exec(State* state, absl::string_view chunk) const;

 private:
  void init(absl::string_view pattern, const Options& options);

  struct DfaBaseParams;
  struct SelectDfaStartStateParams;
  struct DfaLoopParams;

  static bool select_dfa_start_state(SelectDfaStartStateParams* params);
  static bool select_dfa_start_state_impl(SelectDfaStartStateParams* params);

  template <
    bool can_prefix_accel,
    bool run_forward
  >
  static ExecResult dfa_loop_impl(DfaLoopParams* params);

  static inline ExecResult dfa_loop_ff(DfaLoopParams* params);
  static inline ExecResult dfa_loop_ft(DfaLoopParams* params);
  static inline ExecResult dfa_loop_tf(DfaLoopParams* params);
  static inline ExecResult dfa_loop_tt(DfaLoopParams* params);

  static ExecResult dfa_loop(DfaLoopParams* params);
  static inline ExecResult reverse_dfa_loop(DfaLoopParams* params);

 private:
  Options options_;
  re2::Regexp* regexp_;
  re2::Prog* prog_;
  re2::Prog* rprog_;
  std::string error_;
  std::string error_arg_;
  ErrorCode error_code_;
};

class RE2::SM::State {
  friend class RE2::SM;

 protected:
   enum Flags {
     kReverse        = 0x0001, // backward DFA scan to find the start of a match
     kCanPrefixAccel = 0x0002, // can use memchr to fast-forward to a potential match
     kAnchored       = 0x0010, // anchored search
     kInitialized    = 0x0020, // state is initialized
     kFullMatch      = 0x0040, // in this state, DFA matches all the way to the very end
     kMatch          = 0x0100, // match or mismatch; need to reset
     kMismatch       = 0x0200, // match or mismatch; need to reset
   };

 public:
   enum {
     kByteEndText = 256 // same as in DFA::kByteEndText
   };

 public:
  State() {
    reset();
  }
  State(uint64_t base_offset, int prev_char) {
    reset(base_offset, prev_char);
  }
  State(uint64_t base_offset, int prev_char, uint64_t eof_offset) {
    reset(base_offset, prev_char, eof_offset);
  }
  State(uint64_t base_offset, int prev_char, uint64_t eof_offset, int eof_char) {
    reset(base_offset, prev_char, eof_offset, eof_char);
  }

  bool is_match() const {
    return match_id_ != -1;
  }
  uint64_t base_offset() const {
    return base_offset_;
  }
  uint64_t eof_offset() const {
    return eof_offset_;
  }
  uint64_t match_start_offset() const {
    return match_start_offset_;
  }
  uint64_t match_end_offset() const {
    return match_end_offset_;
  }
  uint64_t match_length() const {
    return match_end_offset_ - match_start_offset_;
  }
  uint32_t match_id() const {
    return match_id_;
  }

  void reset() {
    reset(0, kByteEndText, -1, kByteEndText);
  }
  void reset(uint64_t base_offset, int base_char) {
    reset(base_offset, base_char, -1, kByteEndText);
  }
  void reset(uint64_t base_offset, int base_char, uint64_t eof_offset) {
    reset(base_offset, base_char, eof_offset, kByteEndText);
  }
  void reset(uint64_t base_offset, int base_char, uint64_t eof_offset, int eof_char);

  void set_eof(uint64_t offset) {
    set_eof(offset, kByteEndText);
  }

  void set_eof(uint64_t offset, int eof_char);

 private:
  DFA* dfa_;
  void* dfa_state_;       // DFA::State*
  void* dfa_start_state_; // DFA::State*
  uint64_t offset_;
  uint64_t base_offset_;
  uint64_t eof_offset_;
  uint64_t match_start_offset_;
  uint64_t match_end_offset_;
  uint32_t match_id_;
  uint32_t flags_;
  int base_char_;
  int eof_char_;
  int match_end_char_;
  int match_next_char_;
  int last_char_;
};

inline void RE2::SM::State::reset(uint64_t base_offset, int base_char, uint64_t eof_offset, int eof_char) {
  offset_ = base_offset;
  base_offset_ = base_offset;
  eof_offset_ = eof_offset;
  match_start_offset_ = -1;
  match_end_offset_ = -1;
  match_id_ = -1;
  flags_ = 0;
  base_char_ = base_char;
  eof_char_ = eof_char;
  match_end_char_ = base_char;
  match_next_char_ = eof_char;
  last_char_ = base_char;
}

inline void RE2::SM::State::set_eof(uint64_t offset, int eof_char) {
  assert(!(flags_ & kReverse) && "invalid eof on a reverse state");
  assert(offset >= offset_ && "invalid eof offset");
  eof_offset_ = offset;
  eof_char = eof_char;
}

}  // namespace re2

#endif  // RE2_SM_H_
