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
	options.set_posix_syntax(true);
	options.set_longest_match(true);
	options.set_perl_classes(true);
	options.set_word_boundary(true);
	options.set_one_line(false);

/*
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
		printf("matched: %d\n", v[i]); */

	const char pattern[] = "(^abc)";
	const char text[] = "\nabc\n";
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

		re2::RE2::SM sm(pattern, options);
		re2::RE2::SM::State state;
		state.set_eof(length);

		re2::RE2::SM::ExecResult result = sm.exec(&state, text);
		if (result != re2::RE2::SM::kMatch)
			printf("not found\n");
		else
			printf(
				"match at %zd:%zd '%s'\n",
				state.match_start_offset(),
				state.match_end_offset(),
				std::string(text + state.match_start_offset(), state.match_length()).c_str()
			);
	} while (0);

	do {
		printf("using re2::RE2::SM (char-by-char)...\n");

		re2::RE2::SM sm(pattern, options);
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
					"forward-match at %zd:%zd '%s'\n",
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
							"reverse-match at %zd:%zd '%s'\n",
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
