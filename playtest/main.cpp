#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string>

#include "re2.h"
#include "re2/sm.h"
#include "re2/set.h"

void print_re2_sm_submatches(const re2::RE2::SM& sm, int match_id, absl::string_view match) {
	enum {
		MaxSubmatchCount = 8
	};


	const re2::RE2::SM::Module* module = sm.kind() == re2::RE2::SM::kSingleRegexp ? sm.module() : sm.switch_case(match_id);
	size_t capture_count = module->capture_count() + 1;
	assert(capture_count < MaxSubmatchCount);

	absl::string_view submatches[MaxSubmatchCount];
	bool result = module->capture_submatches(match, submatches, capture_count);
	if (!result)
		printf("can't capture submatches\n");
	else
		for (size_t i = 0; i < capture_count; i++)
			printf("  submatch[%zd]: '%s'\n", i, std::string(submatches[i]).c_str());
}

int main() {
	printf("main\n");

	re2::RE2::Options options;
	options.set_longest_match(true);
	options.set_case_sensitive(false);

	const char single_pattern[] = "(?m)(^abc$)";
	const char* switch_patterns[] = {
		"(?m)(^abc$)",
		"(?m)(^def$)",
		"(?m)(^ghi$)",
	};

	const char text[] = " \nghi\n \ndef\n \nabc\n ";
	size_t length = sizeof(text) - 1;
	std::string match;

	do {
		printf("using re2::RE2...\n");

		re2::RE2 re(single_pattern, options);
		absl::string_view match;
		bool result = re.RE2::PartialMatch(text, re, &match);
		if (!result)
			printf("not found\n");
		else
			printf("match at %zd:%zd '%s'\n", match.begin() - text, match.end() - text, std::string(match).c_str());
	} while (0);

	do {
		printf("using re2::RE2::SM (single regexp, full text)...\n");

		re2::RE2::SM sm;
		bool result = sm.create(single_pattern, options);
    if (!result) {
			printf("error: %s\n", sm.error().c_str());
			return -1;
		}

		re2::RE2::SM::State state;
		state.set_eof(length);

		re2::RE2::SM::ExecResult exec_result = sm.exec(&state, text);
		if (exec_result != re2::RE2::SM::kMatch)
			printf("not found\n");
		else {
			match.assign(text + state.match_start_offset(), state.match_length());

			printf(
				"match id: %d at %zd:%zd '%s', \n",
				state.match_id(),
				state.match_start_offset(),
				state.match_end_offset(),
				match.c_str()
			);

			print_re2_sm_submatches(sm, state.match_id(), match);
		}
	} while (0);

	do {
		printf("using re2::RE2::SM (regexp switch, full text)...\n");

		re2::RE2::SM sm;
		sm.create_switch(options);
		for (size_t i = 0; i < sizeof(switch_patterns) / sizeof(switch_patterns[0]); i++)
			sm.add_switch_case(switch_patterns[i]);
		bool result = sm.finalize_switch();
    if (!result) {
			printf("error: %s\n", sm.error().c_str());
			return -1;
		}

		re2::RE2::SM::State state;
		state.set_eof(length);

		re2::RE2::SM::ExecResult exec_result = sm.exec(&state, text);
		if (exec_result != re2::RE2::SM::kMatch)
			printf("not found\n");
		else {
			match.assign(text + state.match_start_offset(), state.match_length());

			printf(
				"match id: %d at %zd:%zd '%s' { 0x%02x; 0x%02x }\n",
				state.match_id(),
				state.match_start_offset(),
				state.match_end_offset(),
				match.c_str(),
				state.match_last_char_,
				state.match_next_char_
			);

			print_re2_sm_submatches(sm, state.match_id(), match);
		}
	} while (0);

	do {
		printf("using re2::RE2::SM (regexp switch, char-by-char)...\n");

		re2::RE2::SM sm;
		sm.create_switch(options);
		for (size_t i = 0; i < sizeof(switch_patterns) / sizeof(switch_patterns[0]); i++)
			sm.add_switch_case(switch_patterns[i]);
		bool result = sm.finalize_switch();
		if (!result) {
			printf("error: %s\n", sm.error().c_str());
			return -1;
		}

		re2::RE2::SM::State state;
		state.set_eof(length);

		const char* p = text;
		const char* eof = text + length;
		while (p < eof) {
			size_t chunk_length = 1;
			re2::RE2::SM::ExecResult result = sm.exec(&state, absl::string_view(p, chunk_length));
			switch (result) {
			case re2::RE2::SM::kContinue:
				p += chunk_length;
				break;

			case re2::RE2::SM::kMismatch:
				printf("mismatch\n");
				return -1;

			case re2::RE2::SM::kMatch:
				match.assign(text + state.match_start_offset(), state.match_length());
				printf(
					"fmatch id: %d at %zd:%zd '%s' { 0x%02x; 0x%02x }\n",
					state.match_id(),
					state.match_start_offset(),
					state.match_end_offset(),
					match.c_str(),
					state.match_last_char_,
					state.match_next_char_
				);

				print_re2_sm_submatches(sm, state.match_id(), match);
				p = text + state.match_end_offset();
				break; // keep searching forward

			case re2::RE2::SM::kContinueBackward:
				printf("end-of-match @%zd\n", state.match_end_offset());

				while (p > text) {
					re2::RE2::SM::ExecResult result = sm.exec(&state, absl::string_view(p - chunk_length, chunk_length));
					switch (result) {
					case re2::RE2::SM::kContinueBackward:
						p -= chunk_length;
						break;

					case re2::RE2::SM::kMatch:
						match.assign(text + state.match_start_offset(), state.match_length());
						printf(
							"rmatch id: %d at %zd:%zd '%s' { 0x%02x; 0x%02x }\n",
							state.match_id(),
							state.match_start_offset(),
							state.match_end_offset(),
							match.c_str(),
							state.match_last_char_,
							state.match_next_char_
						);

						print_re2_sm_submatches(sm, state.match_id(), match);
						p = text; // break out of the parent while loop
						break;

					default:
						assert(false && "unexpected RE2::SM::exec result");
						return -2;
					}
				}

				p = text + state.match_end_offset();
				break;

			default:
				assert(false && "unexpected RE2::SM::exec result");
				return -3;
			}
		}
	} while (0);

	return 0;
}
