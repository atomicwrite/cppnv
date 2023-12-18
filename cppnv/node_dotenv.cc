#include "node_dotenv.h"
#include "env-inl.h"
#include "node_file.h"
#include "uv.h"

namespace node {
using cppnv::EnvKey;
using cppnv::EnvPair;
using cppnv::EnvReader;
using cppnv::EnvStream;
using v8::NewStringType;
using v8::String;

std::vector<std::string> Dotenv::GetPathFromArgs(
    const std::vector<std::string>& args) {
  const auto find_match = [](const std::string& arg) {
    const std::string_view flag = "--env-file";
    return strncmp(arg.c_str(), flag.data(), flag.size()) == 0;
  };
  std::vector<std::string> paths;
  auto path = std::find_if(args.begin(), args.end(), find_match);

  while (path != args.end()) {
    auto equal_char = path->find('=');

    if (equal_char != std::string::npos) {
      paths.push_back(path->substr(equal_char + 1));
    } else {
      auto next_path = std::next(path);

      if (next_path == args.end()) {
        return paths;
      }

      paths.push_back(*next_path);
    }

    path = std::find_if(++path, args.end(), find_match);
  }

  return paths;
}

void Dotenv::SetEnvironment(node::Environment* env) {
  if (store_.empty()) {
    return;
  }

  auto isolate = env->isolate();

  for (const auto& entry : store_) {
    auto key = entry.first;
    auto value = entry.second;

    auto existing = env->env_vars()->Get(key.data());

    if (existing.IsNothing()) {
      env->env_vars()->Set(
          isolate,
          v8::String::NewFromUtf8(
              isolate, key.data(), NewStringType::kNormal, key.size())
              .ToLocalChecked(),
          v8::String::NewFromUtf8(
              isolate, value.data(), NewStringType::kNormal, value.size())
              .ToLocalChecked());
    }
  }
}

bool Dotenv::ParsePath(const std::string_view path) {
  uv_fs_t req;
  auto defer_req_cleanup = OnScopeLeave([&req]() { uv_fs_req_cleanup(&req); });

  uv_file file = uv_fs_open(nullptr, &req, path.data(), 0, 438, nullptr);
  if (req.result < 0) {
    // req will be cleaned up by scope leave.
    return false;
  }
  uv_fs_req_cleanup(&req);

  auto defer_close = OnScopeLeave([file]() {
    uv_fs_t close_req;
    CHECK_EQ(0, uv_fs_close(nullptr, &close_req, file, nullptr));
    uv_fs_req_cleanup(&close_req);
  });

  std::string result{};
  char buffer[8192];
  uv_buf_t buf = uv_buf_init(buffer, sizeof(buffer));

  while (true) {
    auto r = uv_fs_read(nullptr, &req, file, &buf, 1, -1, nullptr);
    if (req.result < 0) {
      // req will be cleaned up by scope leave.
      return false;
    }
    uv_fs_req_cleanup(&req);
    if (r <= 0) {
      break;
    }
    result.append(buf.base, r);
  }

  EnvStream env_stream(&result);

  std::vector<EnvPair*> env_pairs;
  EnvReader::read_pairs(&env_stream, &env_pairs);

  for (const auto pair : env_pairs) {
    EnvReader::finalize_value(pair, &env_pairs);
    store_.insert_or_assign(*pair->key->key, *pair->value->value);
  }
  EnvReader::delete_pairs(&env_pairs);
  return true;
}

void Dotenv::AssignNodeOptionsIfAvailable(std::string* node_options) {
  auto match = store_.find("NODE_OPTIONS");

  if (match != store_.end()) {
    *node_options = match->second;
  }
}

void Dotenv::ParseLine(const std::string_view line) {
  auto equal_index = line.find('=');

  if (equal_index == std::string_view::npos) {
    return;
  }

  auto key = line.substr(0, equal_index);

  // Remove leading and trailing space characters from key.
  while (!key.empty() && std::isspace(key.front())) key.remove_prefix(1);
  while (!key.empty() && std::isspace(key.back())) key.remove_suffix(1);

  // Omit lines with comments
  if (key.front() == '#' || key.empty()) {
    return;
  }

  auto value = std::string(line.substr(equal_index + 1));

  // Might start and end with `"' characters.
  auto quotation_index = value.find_first_of("`\"'");

  if (quotation_index == 0) {
    auto quote_character = value[quotation_index];
    value.erase(0, 1);

    auto end_quotation_index = value.find_last_of(quote_character);

    // We couldn't find the closing quotation character. Terminate.
    if (end_quotation_index == std::string::npos) {
      return;
    }

    value.erase(end_quotation_index);
  } else {
    auto hash_index = value.find('#');

    // Remove any inline comments
    if (hash_index != std::string::npos) {
      value.erase(hash_index);
    }

    // Remove any leading/trailing spaces from unquoted values.
    while (!value.empty() && std::isspace(value.front())) value.erase(0, 1);
    while (!value.empty() && std::isspace(value.back()))
      value.erase(value.size() - 1);
  }

  store_.insert_or_assign(std::string(key), value);
}

}  // namespace node


namespace cppnv {
VariablePosition::VariablePosition(const int variable_start,
                                   const int start_brace,
                                   const int dollar_sign)
  : variable_start(
        variable_start),
    start_brace(start_brace),
    dollar_sign(dollar_sign),
    end_brace(0),
    variable_end(0) {
  variable_str = new std::string();
}
cppnv::EnvStream::EnvStream(std::string* data) {
  this->data_ = data;
  this->length_ = this->data_->length();
  this->is_good_ = this->index_ < this->length_;
}

char cppnv::EnvStream::get() {
  if (this->index_ >= this->length_) {
    return -1;
  }
  const auto ret = this->data_->at(this->index_);
  this->index_++;
  this->is_good_ = this->index_ < this->length_;
  return ret;
}

bool cppnv::EnvStream::good() const {
  return this->is_good_;
}

bool cppnv::EnvStream::eof() const {
  return !good();
}
EnvReader::read_result EnvReader::read_pair(EnvStream* file,
                                            const EnvPair* pair) {
  const read_result result = read_key(file, pair->key);
  if (result == fail || result == empty) {
    return fail;
  }
  if (result == comment_encountered) {
    return comment_encountered;
  }
  if (result == end_of_stream_key) {
    return end_of_stream_key;
  }

  if (result == end_of_stream_value) {
    return success;
  }
  //  trim right side of key
  while (pair->key->key_index > 0) {
    if (pair->key->key->at(pair->key->key_index - 1) != ' ') {
      break;
    }
    pair->key->key_index--;
  }
  if (!pair->key->has_own_buffer()) {
    const auto tmp_str = new std::string(pair->key->key_index, '\0');

    tmp_str->replace(0,
                     pair->key->key_index,
                     *pair->key->key,
                     0,
                     pair->key->key_index);

    pair->key->set_own_buffer(tmp_str);;
  } else {
    pair->key->clip_own_buffer(pair->key->key_index);
  }
  pair->value->value->clear();
  const read_result value_result = read_value(file, pair->value);
  if (value_result == end_of_stream_value) {
    return end_of_stream_value;
  }
  if (value_result == comment_encountered || value_result == success) {
    if (!pair->value->has_own_buffer()) {
      const auto tmp_str =
          new std::string(pair->value->value_index, '\0');

      tmp_str->replace(0,
                       pair->value->value_index,
                       *pair->value->value,
                       0,
                       pair->value->value_index);

      pair->value->
            set_own_buffer(tmp_str);
    } else {
      pair->value->clip_own_buffer(pair->value->value_index);
    }
    remove_unclosed_interpolation(pair->value);
    return success;
  }
  if (value_result == empty) {
    remove_unclosed_interpolation(pair->value);
    return empty;
  }
  if (value_result == end_of_stream_key) {
    remove_unclosed_interpolation(pair->value);
    return end_of_stream_key;
  }

  remove_unclosed_interpolation(pair->value);
  return fail;
}


int EnvReader::read_pairs(EnvStream* file, std::vector<EnvPair*>* pairs) {
  int count = 0;
  auto buffer = std::string(256, '\0');

  while (true) {
    buffer.clear();
    EnvPair* pair = new EnvPair();
    pair->key = new EnvKey();
    pair->key->key = &buffer;
    pair->value = new EnvValue();
    pair->value->value = &buffer;
    const read_result result = read_pair(file, pair);
    if (result == end_of_stream_value) {
      pairs->push_back(pair);
      count++;
      break;
    }
    if (result == success) {
      pairs->push_back(pair);
      count++;
      continue;
    }

    delete pair->key;
    delete pair->value;
    delete pair;
    if (result == comment_encountered || result == fail) {
      continue;
    }
    break;
  }

  return count;
}

void EnvReader::delete_pair(const EnvPair* pair) {
  delete pair->key;
  delete pair->value;
  delete pair;
}

void EnvReader::delete_pairs(const std::vector<EnvPair*>* pairs) {
  for (const auto env_pair : *pairs) {
    delete_pair(env_pair);
  }
}


void EnvReader::clear_garbage(EnvStream* file) {
  char key_char;
  do {
    key_char = file->get();
    if (key_char < 0) {
      break;
    }
    if (!file->good()) {
      break;
    }
  } while (key_char != '\n');
}

EnvReader::read_result EnvReader::position_of_dollar_last_sign(
    const EnvValue* value,
    int* position) {
  if (value->value_index < 1) {
    return empty;
  }
  auto tmp = value->value_index - 2;

  while (tmp >= 0) {
    if (value->value->at(tmp) == '$') {
      if (tmp > 0 && value->value->at(tmp - 1) == '\\') {
        return fail;
      }
      break;
    }
    if (value->value->at(tmp) == ' ') {
      tmp = tmp - 1;
      continue;
    }
    return fail;
  }
  *position = tmp;
  return success;
}

/**
 * \brief Assumes you've swept to new line before this and reads in a key.
 * \breif Anything is legal except newlines or =
 * \param file
 * \param key
 * \return
 */
EnvReader::read_result EnvReader::read_key(EnvStream* file, EnvKey* key) {
  if (!file->good()) {
    return end_of_stream_key;
  }

  while (file->good()) {
    const auto key_char = file->get();
    if (key_char < 0) {
      break;
    }
    if (key_char == '#') {
      clear_garbage(file);
      return comment_encountered;
    }
    switch (key_char) {
      case ' ':
        if (key->key_index == 0) {
          continue;  // left trim keys
        }
        key->key->push_back(key_char);
        key->key_index++;  // I choose to support things like abc dc=ef
        break;
      case '=':
        if (!file->good()) {
          return end_of_stream_value;
        }
        return success;
      case '\r':
        continue;
      case '\n':
        return fail;
      default:
        key->key->push_back(key_char);
        key->key_index++;
    }
    if (!file->good()) {
      break;
    }
  }
  return end_of_stream_key;
}

int EnvReader::get_white_space_offset_left(const std::string* value,
                                           const VariablePosition*
                                           interpolation) {
  int tmp = interpolation->variable_start;
  int size = 0;
  while (tmp >= interpolation->start_brace) {
    if (value->at(tmp) != ' ') {
      break;
    }
    tmp = tmp - 1;
    size = size + 1;
  }
  return size;
}

int EnvReader::get_white_space_offset_right(const std::string* value,
                                            const VariablePosition*
                                            interpolation) {
  int tmp = interpolation->end_brace - 1;
  int count = 0;
  while (tmp >= interpolation->start_brace) {
    if (value->at(tmp) != ' ') {
      break;
    }
    count = count + 1;
    tmp = tmp - 1;
  }
  return count;
}

bool EnvReader::process_possible_control_character(
    EnvValue* value,
    const char key_char) {
  switch (key_char) {
    case '\0':
      return false;
    case 'v':

      add_to_buffer(value, '\v');
      return true;
    case 'a':

      add_to_buffer(value, '\a');
      return true;
    case 't':

      add_to_buffer(value, '\t');
      return true;
    case 'n':
      add_to_buffer(value, '\n');
      return true;
    case 'r':
      add_to_buffer(value, '\r');
      return true;
    case '"':
      add_to_buffer(value, '"');
      return true;
    case 'b':
      add_to_buffer(value, '\b');
      return true;
    case '\'':
      add_to_buffer(value, '\'');
      return true;
    case '\f':
      add_to_buffer(value, '\f');
      return true;
    default:
      return false;
  }
}

void EnvReader::walk_back_slashes(EnvValue* value) {
  if (const int total_backslash_pairs = value->back_slash_streak / 2;
    total_backslash_pairs > 0) {
    for (int i = 0; i < total_backslash_pairs; i++) {
      add_to_buffer(value, '\\');
    }
    value->back_slash_streak -= total_backslash_pairs * 2;
  }
}

void EnvReader::close_variable(EnvValue* value) {
  value->is_parsing_variable = false;
  VariablePosition* const interpolation = value->interpolations->at(
      value->interpolation_index);
  interpolation->end_brace = value->value_index - 1;
  interpolation->variable_end = value->value_index - 2;
  if (const auto left_whitespace = get_white_space_offset_left(
      value->value,
      interpolation); left_whitespace > 0) {
    interpolation->variable_start =
        interpolation->variable_start + left_whitespace;
  }
  if (const auto right_whitespace = get_white_space_offset_right(
      value->value,
      interpolation); right_whitespace > 0) {
    interpolation->variable_end =
        interpolation->variable_end - right_whitespace;
  }
  const auto variable_len = (interpolation->variable_end - interpolation->
                             variable_start) + 1;
  interpolation->variable_str->resize(variable_len);
  interpolation->variable_str->replace(0,
                                       variable_len,
                                       *value->value,
                                       interpolation->variable_start,
                                       variable_len);
  interpolation->closed = true;
  value->interpolation_index++;
}

void EnvReader::open_variable(EnvValue* value) {
  int position;
  const auto result = position_of_dollar_last_sign(value, &position);

  if (result == success) {
    value->is_parsing_variable = true;
    value->interpolations->push_back(
        new VariablePosition(value->value_index,
                             value->value_index - 1,
                             position));
  }
}

bool EnvReader::walk_double_quotes(EnvValue* value) {
  // we have have some quotes at the start
  if (value->value_index == 0) {
    if (value->double_quote_streak == 1) {
      value->double_quote_streak = 0;
      value->double_quoted = true;
      return false;  // we have a double quote at the start
    }
    // we have a empty double quote value aka ''
    if (value->double_quote_streak == 2) {
      value->double_quote_streak = 0;
      value->double_quoted = true;
      return true;  // we have a  empty double quote at the start
    }
    if (value->double_quote_streak == 3) {
      value->double_quote_streak = 0;
      value->triple_double_quoted = true;
      return false;  // we have a triple double quote at the start
    }
    if (value->double_quote_streak > 5) {
      value->double_quote_streak = 0;
      value->triple_double_quoted = true;
      // basically we have """""" an empty heredoc with extra " at the end.
      // Ignore the trailing "
      return true;  // we have a triple quote at the start
    }
    if (value->double_quote_streak > 3) {
      value->triple_double_quoted = true;
      // we have """"...
      // add the diff to the buffer
      for (int i = 0; i < value->double_quote_streak - 3; i++) {
        add_to_buffer(value, '"');
      }
      value->double_quote_streak = 0;
      return false;  // we have a triple quote at the start
    }

    return false;  // we have garbage
  }

  // we're single quoted
  if (value->double_quoted) {
    // any amount of quotes sends
    value->double_quote_streak = 0;
    return true;
  }

  // We have a triple quote
  if (value->triple_double_quoted) {
    if (value->double_quote_streak == 3 || value->double_quote_streak > 3) {
      value->double_quote_streak = 0;
      return true;  // we have enough to close, truncate trailing single quotes
    }
    // we have not enough to close the heredoc.
    if (value->double_quote_streak < 3) {
      // add them to the buffer
      for (int i = 0; i < value->double_quote_streak; i++) {
        add_to_buffer(value, '"');
      }
      value->double_quote_streak = 0;
      return false;
    }
    return false;  // we have garbage
  }

  return false;
}

/**
 * \brief
 * \param value
 * \return True if end quotes detected and input should stop, false otherwise
 */
bool EnvReader::walk_single_quotes(EnvValue* value) {
  // we have have some quotes at the start
  if (value->value_index == 0) {
    if (value->single_quote_streak == 1) {
      value->single_quote_streak = 0;
      value->quoted = true;
      return false;  // we have a single quote at the start
    }
    // we have a empty single quote value aka ''
    if (value->single_quote_streak == 2) {
      value->single_quote_streak = 0;
      value->quoted = true;
      return true;  // we have a  empty quote at the start
    }
    if (value->single_quote_streak == 3) {
      value->single_quote_streak = 0;
      value->triple_quoted = true;
      return false;  // we have a triple quote at the start
    }
    if (value->single_quote_streak > 5) {
      value->single_quote_streak = 0;
      value->triple_quoted = true;
      // basically we have '''''' an empty heredoc with extra ' at the end.
      // Ignore the trailing '
      return true;  // we have a triple quote at the start
    }
    if (value->single_quote_streak > 3) {
      value->triple_quoted = true;
      // we have ''''...
      // add the diff to the buffer
      for (int i = 0; i < value->single_quote_streak - 3; i++) {
        add_to_buffer(value, '\'');
      }
      value->single_quote_streak = 0;
      return false;  // we have a triple quote at the start
    }

    return false;  // we have garbage
  }

  // we're single quoted
  if (value->quoted) {
    // any amount of quotes sends
    value->single_quote_streak = 0;
    return true;
  }

  // We have a triple quote
  if (value->triple_quoted) {
    if (value->single_quote_streak == 3 || value->single_quote_streak > 3) {
      value->single_quote_streak = 0;
      return true;  // we have enough to close, truncate trailing single quotes
    }
    // we have not enough to close the heredoc.
    if (value->single_quote_streak < 3) {
      // add them to the buffer
      for (int i = 0; i < value->single_quote_streak; i++) {
        add_to_buffer(value, '\'');
      }
      value->single_quote_streak = 0;
      return false;
    }
    return false;  // we have garbage
  }

  return false;
}


void EnvReader::add_to_buffer(EnvValue* value, const char key_char) {
  size_t size = value->value->size();
  if (static_cast<size_t>(value->value_index) >= size) {
    if (size == 0) {
      size = 100;
    }
    value->value->resize(size * 150 / 100);
  }
  (*value->value)[value->value_index] = key_char;
  value->value_index++;
}

bool EnvReader::read_next_char(EnvValue* value, const char key_char) {
  if (!value->quoted && !value->triple_quoted && value->back_slash_streak > 0) {
    if (key_char != '\\') {
      walk_back_slashes(value);
      if (value->back_slash_streak == 1) {
        // do we have an odd backslash out? ok, process control char
        value->back_slash_streak = 0;
        if (process_possible_control_character(value, key_char)) {
          return true;
        }
        add_to_buffer(value, '\\');
      }
    }
  }
  if (!value->triple_double_quoted && !value->double_quoted && value->
      single_quote_streak > 0) {
    if (key_char != '\'') {
      if (walk_single_quotes(value)) {
        return false;
      }
    }
  }
  if (!value->triple_quoted && !value->quoted && value->
      double_quote_streak > 0) {
    if (key_char != '"') {
      if (walk_double_quotes(value)) {
        return false;
      }
    }
  }
  // Check to see if the first character is a ' or ". If it is neither,
  // it is an implicit double quote.
  if (value->value_index == 0) {
    if (key_char == '`') {
      if (value->back_tick_quoted) {
        return false;
      }
      if (!value->quoted && !value->triple_quoted && !value->double_quoted &&
          !value->triple_double_quoted) {
        value->double_quoted = true;
        value->back_tick_quoted = true;
        return true;
      }
    }

    if (key_char == '#') {
      if (!value->quoted && !value->triple_quoted && !value->double_quoted &&
          !value->triple_double_quoted) {
        return false;
      }
    } else if (!(key_char == '"' || key_char == '\'')) {
      if (!value->quoted && !value->triple_quoted && !value->double_quoted
          && !value->triple_double_quoted) {
        value->double_quoted = true;
        value->implicit_double_quote = true;
      }
    }
    if (key_char == ' ' && value->implicit_double_quote) {
      return true;  // trim left strings on implicit quotes
    }
  }
  switch (key_char) {
    case '`':
      if (value->back_tick_quoted) {
        return false;
      }
      add_to_buffer(value, key_char);
      break;
    case '#':
      if (value->implicit_double_quote) {
        return false;
      }
      add_to_buffer(value, key_char);
      break;
    case '\n':
      if (!(value->triple_double_quoted || value->triple_quoted || (value->
              double_quoted && !value->implicit_double_quote))) {
        if (value->value_index > 0) {
          if (value->value->at(value->value_index - 1) == '\r') {
            value->value_index--;
          }
        }
        return false;
      }
      add_to_buffer(value, key_char);
      return true;
    case '\\':

      if (value->quoted || value->triple_quoted) {
        add_to_buffer(value, key_char);
        return true;
      }
      value->back_slash_streak++;

      return true;
    case '{':
      add_to_buffer(value, key_char);
      if (!value->quoted && !value->triple_quoted) {
        if (!value->is_parsing_variable) {
          // check to see if it's an escaped '{'
          if (!is_previous_char_an_escape(value)) {
            open_variable(value);
          }
        }
      }
      return true;
    case '}':
      add_to_buffer(value, key_char);
      if (value->is_parsing_variable) {
        // check to see if it's an escaped '}'
        if (!is_previous_char_an_escape(value)) {
          close_variable(value);
        }
      }

      return true;
    case '\'':
      if (!value->double_quoted && !value->triple_double_quoted) {
        value->single_quote_streak++;
      } else {
        add_to_buffer(value, key_char);
      }
      return true;

    case '"':
      if (!value->quoted && !value->triple_quoted && !value->back_tick_quoted &&
          !value->implicit_double_quote) {
        value->double_quote_streak++;
      } else {
        add_to_buffer(value, key_char);
      }
      return true;

    default:
      add_to_buffer(value, key_char);
  }
  return true;
}

// Used only when checking closed and open variables because the { }
// // have been added to the buffer
// // it needs to check 2 values back.
bool EnvReader::is_previous_char_an_escape(const EnvValue* value) {
  return value->value_index > 1
         && value->value->at(value->value_index - 2) ==
         '\\';
}


EnvReader::read_result EnvReader::read_value(EnvStream* file,
                                             EnvValue* value) {
  if (!file->good()) {
    return end_of_stream_value;
  }

  char key_char = 0;
  while (file->good()) {
    key_char = file->get();
    if (key_char < 0) {
      break;
    }

    if (read_next_char(value, key_char) && file->good()) {
      continue;
    }
    break;
  }
  if (value->back_slash_streak > 0) {
    walk_back_slashes(value);
    if (value->back_slash_streak == 1) {
      process_possible_control_character(value, '\0');
    }
  }
  if (value->single_quote_streak > 0) {
    if (walk_single_quotes(value)) {
      if (key_char != '\n') {
        clear_garbage(file);
      }
    }
  }
  if ((value->triple_double_quoted || value->triple_quoted)
    && key_char != '\n') {
    clear_garbage(file);
  }
  if (value->double_quote_streak > 0) {
    if (walk_double_quotes(value)) {
      if (key_char != '\n') {
        clear_garbage(file);
      }
    }
  }
  // trim right side of implicit double quote
  if (value->implicit_double_quote) {
    while (value->value_index > 0) {
      if (value->value->at(value->value_index - 1) != ' ') {
        break;
      }
      value->value_index--;
    }
  }
  return success;
}

void EnvReader::remove_unclosed_interpolation(EnvValue* value) {
  for (int i = value->interpolation_index - 1; i >= 0; i--) {
    const VariablePosition* interpolation = value->interpolations->at(i);
    if (interpolation->closed) {
      continue;
    }
    value->interpolations->erase(
        value->interpolations->begin() + value->interpolation_index);
    delete interpolation;
    value->interpolation_index--;
  }
}

EnvReader::finalize_result EnvReader::finalize_value(
    const EnvPair* pair,
    std::vector<EnvPair*>* pairs) {
  if (pair->value->interpolation_index == 0) {
    pair->value->is_already_interpolated = true;
    pair->value->is_being_interpolated = false;
    return copied;
  }
  pair->value->is_being_interpolated = true;
  const auto buffer = new std::string(*pair->value->value);

  pair->value->set_own_buffer(buffer);

  const auto size = static_cast<int>(pair->value->interpolations->size());
  for (auto i = size - 1; i >= 0; i--) {
    const VariablePosition* interpolation = pair->value->interpolations->at(i);

    for (const EnvPair* other_pair : *pairs) {
      const size_t variable_str_len =
          static_cast<size_t>(interpolation->variable_end) - interpolation->
          variable_start + 1;
      if (variable_str_len != other_pair->key->
                                          key->size()) {
        continue;
      }

      if (0 != memcmp(other_pair->key->key->data(),
                      pair->value->value->data() + interpolation->
                      variable_start,
                      variable_str_len))
        continue;
      if (other_pair->value->is_being_interpolated) {
        return circular;
      }
      if (!other_pair->value->is_already_interpolated) {
        const auto walk_result = finalize_value(other_pair, pairs);
        if (walk_result == circular) {
          return circular;
        }
      }
      buffer->replace(interpolation->dollar_sign,
                      (interpolation->end_brace
                       - interpolation->dollar_sign) +
                      1,
                      *other_pair->value->value);

      break;
    }
  }
  pair->value->is_already_interpolated = true;
  pair->value->is_being_interpolated = false;
  return interpolated;
}
}  // namespace cppnv
