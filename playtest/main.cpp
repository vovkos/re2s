#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string>

#include "re2.h"
#include "re2/sm.h"
#include "re2/set.h"

int main() {
	printf("main\n");

	re2::RE2::Options options;
	options.set_longest_match(true);
	options.set_case_sensitive(false);

	/* do {
		const char* patterns[] = {
			"a?b+c*",
			"i*n+t?",
			"[a-z]+"
		};

		RE2::Set set(options, re2::RE2::UNANCHORED);
		std::string error;

		for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
			int k = set.Add(patterns[i], &error);
			printf("%s -> %d\n", patterns[i], k);
		}

		bool result = set.Compile();
		assert(result);

		const char* text = "abc";

		std::vector<int> v;
		result = set.Match(text, &v);
		assert(result);

		size_t count = v.size();
		for (size_t i = 0; i < count; i++)
			printf("matched: %d\n", v[i]);
	} while (0); */

	const char pattern[] = "(?m)(abc|def|ghi)";
	const char text[] = "  ghi   def   abc ";
	size_t length = sizeof(text) - 1;

	do {
		printf("using re2::RE2...\n");

		re2::RE2 re(pattern, options);
		absl::string_view match;
		bool result = re.RE2::PartialMatch(text, re, &match);
		if (!result)
			printf("not found\n");
		else
			printf("match at %zd:%zd '%s'\n", match.begin() - text, match.end() - text, std::string(match).c_str());
	} while (0);

	do {
		printf("using re2::RE2::SM (full text)...\n");

		re2::RE2::SM sm("ghi", options);
		sm.create_switch(options);
		sm.add_switch_case("abc");
		sm.add_switch_case("def");
		sm.add_switch_case("ghi");
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
		else
			printf(
				"match id: %d at %zd:%zd '%s'\n",
				state.match_id(),
				state.match_start_offset(),
				state.match_end_offset(),
				std::string(text + state.match_start_offset(), state.match_length()).c_str()
			);
	} while (0);

	do {
		printf("using re2::RE2::SM (char-by-char)...\n");

		re2::RE2::SM sm;
		sm.create_switch(options);
		sm.add_switch_case("abc");
		sm.add_switch_case("def");
		sm.add_switch_case("ghi");
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
				printf(
					"fmatch id: %d at %zd:%zd '%s'\n",
					state.match_id(),
					state.match_start_offset(),
					state.match_end_offset(),
					std::string(text + state.match_start_offset(), state.match_length()).c_str()
				);

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
						printf(
							"rmatch id: %d at %zd:%zd '%s'\n",
							state.match_id(),
							state.match_start_offset(),
							state.match_end_offset(),
							std::string(text + state.match_start_offset(), state.match_length()).c_str()
						);

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
