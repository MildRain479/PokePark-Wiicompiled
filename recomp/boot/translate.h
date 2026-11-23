/* The port's own words (the Options book, the second screen, the language
 * pack and save messages, the touch controls) in the language the game's
 * text started in (g_language_started): tr() takes the English and gives
 * it back translated, or as it was if it has no translation. Text is
 * UTF-8; the Japanese is kana alone, as the Japanese font has few kanji. */
#pragma once

const char* tr(const char* english);
