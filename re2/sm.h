// Copyright 2010 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef RE2_SM_H_
#define RE2_SM_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "re2/re2.h"

namespace re2 {
class Prog;
class Regexp;
class DFA;
}  // namespace re2

namespace re2 {

class RE2::SM {
 public:
  class State;

  enum ExecResult {
	  kError    = -1, // out-of-memory or other error
	  kContinue = 0,  // match not found yet; continue feeding data
	  kReverse,       // match end found; reverse & continue feeding backward data
	  kMatch,         // match end & start found
	  kMismatch,      // match can't be found; next exec will reset & restart
  };

 public:
  explicit SM(const char* pattern);
  explicit SM(const std::string& pattern);
  explicit SM(absl::string_view pattern);
  SM(absl::string_view pattern, const Options& options);
  ~SM();

  bool ok() const { return error_code_ == NoError; }

  ErrorCode error_code() const { return error_code_; }
  const std::string& error() const { return error_; }
  const std::string& error_arg() const { return error_arg_; }

  static ExecResult Exec(absl::string_view chunk, const SM& sm, State* state) {
    return sm.exec(chunk, state);
  }

  ExecResult exec(absl::string_view chunk, State* state) const;

 private:
  void Init(absl::string_view pattern, const Options& options);

  struct DfaLoopParams;

  template <
    bool can_prefix_accel,
    bool run_forward
  >
  ExecResult dfaLoopImpl(DfaLoopParams* params) const;

  inline ExecResult dfaLoop_ff(DfaLoopParams* params) const;
  inline ExecResult dfaLoop_ft(DfaLoopParams* params) const;
  inline ExecResult dfaLoop_tf(DfaLoopParams* params) const;
  inline ExecResult dfaLoop_tt(DfaLoopParams* params) const;

  ExecResult dfaLoop(DfaLoopParams* params) const;

 private:
  Options options_;
  re2::Regexp* regexp_;
  std::unique_ptr<re2::Prog> prog_;
  std::unique_ptr<re2::Prog> rprog_;
  std::string error_;
  std::string error_arg_;
  ErrorCode error_code_;
};

class RE2::SM::State {
  friend class RE2::SM;

 protected:
   enum Flags {
     kReverse   = 0x01, // backward DFA scan to find the start of a match
     kFinal     = 0x02, // match or mismatch; need to reset
     kFullMatch = 0x04, // in this state, DFA matches all the way to the very end
     kEof       = 0x08, // the upcoming chunk is the last one (end-of-file)
   };

 public:
  State() { reset(); }
  State(int prev_char, uint64_t offset) { reset(prev_char, offset); }

  bool is_match() const { return match_id_ != -1; }
  uint64_t offset() const { return offset_; }
  uint64_t base_offset() const { return base_offset_; }
  uint64_t match_start_offset() const { return match_start_offset_; }
  uint64_t match_end_offset() const { return match_end_offset_; }
  uint64_t match_length() const { return match_end_offset_ - match_start_offset_; }
  size_t match_id() const { return match_id_; }
  size_t consumed_size() const { return consumed_size_; }

  void reset() { reset(0, 0); }
  void reset(int prev_char, uint64_t offset);

  void set_eof() {
    assert(!(flags_ & kReverse) && "invalid eof on a reverse state");
    flags_ |= kEof;
  }

 private:
  void* dfa_state_; // DFA::State*
  uint64_t offset_;
  uint64_t base_offset_;
  uint64_t match_start_offset_;
  uint64_t match_end_offset_;
  uint64_t last_match_end_offset_;
  size_t match_id_;
  size_t consumed_size_;
  uintptr_t flags_;
  int prev_char_;
};

inline void RE2::SM::State::reset(int prev_char, uint64_t offset) {
  offset_ = offset;
  base_offset_ = offset;
  match_start_offset_ = -1;
  match_end_offset_ = -1;
  match_id_ = -1;
  consumed_size_ = 0;
  flags_ = 0;
  prev_char_ = prev_char;
}

}  // namespace re2

#endif  // RE2_SM_H_
