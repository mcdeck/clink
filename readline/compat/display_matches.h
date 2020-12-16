#pragma once

struct match_display_filter_entry
{
    int visible_len;    // Visible characters, not counting ANSI escape codes.
    char match[1];      // Variable length string field, NUL terminated.
};
typedef struct match_display_filter_entry match_display_filter_entry;

// Match display filter entry [0] is a placeholder and is ignored except in two
// ways:
//  1.  If the entry is nullptr, the list is empty.
//  2.  If its visible_len is negative, then force the list to be displayed in a
//      single column.
typedef match_display_filter_entry** rl_match_display_filter_func_t(char**);
extern rl_match_display_filter_func_t *rl_match_display_filter_func;

extern const char *_rl_filtered_color;

extern void display_matches(char **matches);
