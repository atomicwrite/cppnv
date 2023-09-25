﻿#pragma once

#include <istream>
#include <map>
#include <vector>

#include "EnvPair.h"

class env_reader
{
public:
    enum read_result { success, empty, fail, comment_encountered, end_of_stream_key, end_of_stream_value };

    enum finalize_result { interpolated, copied, circular };

private:
    static read_result position_of_dollar_last_sign(const env_value* value, int* position);
    static read_result read_key(std::istream& file, env_key* key);
    static int get_white_space_offset_left(const std::string* value, const   variable_position* interpolation);

    static int get_white_space_offset_right(const std::string* value, const   variable_position* interpolation);
    static bool process_possible_control_character(env_value* value, const char key_Char);
    static bool walk_back_slashes(env_value* value, char key_char);
    static void close_variable(env_value* value);
    static void open_variable(env_value* value);
    static bool walk_double_quotes(env_value* value);
    static bool walk_single_quotes(env_value* value);
    static void add_to_buffer(env_value* value, char key_char);
    static bool read_next_char(env_value* value, char key_char);
    static bool clear_newline_or_comment(std::istream& file, env_value* value, char key_char, read_result& ret_value);
    static read_result read_value(std::istream& file, env_value* value);
    

public:
    static finalize_result finalize_value(const env_pair* pair, std::map<std::string, env_pair *>* map);
    static finalize_result finalize_value(const env_pair* pair, std::vector<env_pair*>* pairs);
    static read_result read_pair(std::istream& file, const env_pair* pair);
    static void create_pair(std::string* buffer, env_pair*& pair);
    static int read_pairs(std::istream& file, std::vector<env_pair*>* pairs);
    static void delete_pair(const env_pair* pair);
    static void delete_pairs(const std::vector<env_pair*>* pairs);
    static int read_pairs(  std::istream& file, std::map<std::string, env_pair *>* map);
};
