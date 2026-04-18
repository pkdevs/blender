/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2002-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 *
 * \brief Parser for DNA header files used by `makesdna`.
 *
 * The parser understands a basic subset of C/C++ needed for parsing structs
 * and their members. Code surrounded by `#ifdef __cplusplus` is skipped to
 * avoid having to parse more advanced C++.
 */

#include "dna_parse.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_alloca.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "dna_utils.h"

namespace blender {

extern int debugSDNA;

namespace dna {

#define DEBUG_PRINTF(debug_level, ...) \
  { \
    if (debugSDNA > debug_level) { \
      printf(__VA_ARGS__); \
    } \
  } \
  ((void)0)

/* -------------------------------------------------------------------- */
/** \name Identifier and Preprocessor Matching
 * \{ */

static bool match_identifier_with_len(const char *str,
                                      const char *identifier,
                                      const size_t identifier_len)
{
  if (strncmp(str, identifier, identifier_len) == 0) {
    /* Check `str` isn't a prefix to a longer identifier. */
    if (isdigit(str[identifier_len]) || isalpha(str[identifier_len]) ||
        (str[identifier_len] == '_'))
    {
      return false;
    }
    return true;
  }
  return false;
}

static bool match_identifier(const char *str, const char *identifier)
{
  const size_t identifier_len = strlen(identifier);
  return match_identifier_with_len(str, identifier, identifier_len);
}

static bool match_identifier_and_advance(char **str_ptr, const char *identifier)
{
  const size_t identifier_len = strlen(identifier);
  if (match_identifier_with_len(*str_ptr, identifier, identifier_len)) {
    (*str_ptr) += identifier_len;
    return true;
  }
  return false;
}

/* Copied from `BLI_str_startswith` string.c
 * to avoid complicating the compilation process of makesdna. */
static bool str_startswith(const char *__restrict str, const char *__restrict start)
{
  for (; *str && *start; str++, start++) {
    if (*str != *start) {
      return false;
    }
  }

  return (*start == '\0');
}

/**
 * Check if `str` is a preprocessor string that starts with `start`.
 * The `start` doesn't need the `#` prefix.
 * `ifdef VALUE` will match `#ifdef VALUE` as well as `#  ifdef VALUE`.
 */
static bool match_preproc_prefix(const char *__restrict str, const char *__restrict start)
{
  if (*str != '#') {
    return false;
  }
  str++;
  while (*str == ' ') {
    str++;
  }
  return str_startswith(str, start);
}

/**
 * \return The point in `str` that starts with `start` or nullptr when not found.
 */
static char *match_preproc_strstr(char *__restrict str, const char *__restrict start)
{
  while ((str = strchr(str, '#'))) {
    str++;
    while (*str == ' ') {
      str++;
    }
    if (str_startswith(str, start)) {
      return str;
    }
  }
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Name Validation
 * \{ */

/**
 * Validate a type name used for a struct definition or a member's type.
 *
 * Rejects empty names, names containing `*` (e.g. `struct Foo*` — valid C but
 * unsupported here), and `long`/`ulong` whose size differs across platforms.
 *
 * \return true if valid. Prints an error with \a filepath on failure.
 */
static bool is_valid_type_name(const char *type_name, const char *filepath)
{
  if (type_name[0] == 0 || strchr(type_name, '*')) {
    fprintf(stderr, "File '%s' contains struct we can't parse \"%s\"\n", filepath, type_name);
    return false;
  }
  if (STR_ELEM(type_name, "long", "ulong")) {
    fprintf(stderr,
            "File '%s' contains use of \"%s\" in DNA struct which is not allowed\n",
            filepath,
            type_name);
    return false;
  }
  return true;
}

/**
 * Enforce the `_pad123` member-name convention:
 * - Only `_pad*` members may start with an underscore.
 * - Bare `pad123` / `pad_123` is rejected (use `_pad123`).
 * - `pad` followed by `[a-z]` is allowed (e.g. `pad_rot_angle`).
 *
 * \return true if valid. Prints an error with \a filepath on failure.
 */
static bool is_valid_member_name(const char *name, const char *filepath)
{
  const int name_size = strlen(name) + 1;
  char *name_strip = static_cast<char *>(alloca(name_size));
  DNA_member_id_strip_copy(name_strip, name);

  const char prefix[] = {'p', 'a', 'd'};

  if (name[0] == '_') {
    if (strncmp(&name_strip[1], prefix, sizeof(prefix)) != 0) {
      fprintf(stderr,
              "File '%s': only '_pad' variables can start with an underscore, found '%s'\n",
              filepath,
              name);
      return false;
    }
  }
  else if (strncmp(name_strip, prefix, sizeof(prefix)) == 0) {
    int i = sizeof(prefix);
    if (name_strip[i] >= 'a' && name_strip[i] <= 'z') {
      /* may be part of a word, allow that. */
      return true;
    }
    bool has_only_digit_or_none = true;
    for (; name_strip[i]; i++) {
      const char c = name_strip[i];
      if (!((c >= '0' && c <= '9') || c == '_')) {
        has_only_digit_or_none = false;
        break;
      }
    }
    if (has_only_digit_or_none) {
      /* found 'pad' or 'pad123'. */
      fprintf(stderr,
              "File '%s': padding variables must be formatted '_pad[number]', found '%s'\n",
              filepath,
              name);
      return false;
    }
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Source Buffer Preprocessing
 * \{ */

/**
 * Remove comments from this buffer. Assumes that the buffer refers to
 * ASCII-code text.
 */
static int preprocess_dna_header(char *maindata, const int maindata_len)
{
  /* NOTE: len + 1, last character is a dummy to prevent
   * comparisons using uninitialized memory */
  char *temp = MEM_new_array_uninitialized<char>(size_t(maindata_len) + 1,
                                                 "preprocess_dna_header");
  temp[maindata_len] = ' ';

  memcpy(temp, maindata, maindata_len);

  /* remove all strings literals and comments */
  /* replace all enters/tabs/etc with spaces */
  bool c_comment = false;
  bool cxx_comment = false;
  bool string_literal = false;
  for (char *cp = temp; cp < temp + maindata_len; cp++) {
    if (string_literal) {
      if (cp[0] == '"') {
        /* End string literal */
        string_literal = false;
      }
      else if (cp[0] == '\\' && cp[1] == '"') {
        /* Skip escaped quote in string literal. */
        cp[0] = ' ';
        cp++;
      }
      cp[0] = ' ';
    }
    else if (cxx_comment) {
      if (*cp == '\n') {
        cxx_comment = false;
      }
      cp[0] = ' ';
    }
    else if (c_comment) {
      if (cp[0] == '*' && cp[1] == '/') {
        /* End C comment */
        c_comment = false;
        cp[0] = ' ';
        cp++;
      }
      cp[0] = ' ';
    }
    else if (cp[0] == '"') {
      /* Start string literal. */
      string_literal = true;
      cp[0] = ' ';
    }
    else if (cp[0] == '/' && cp[1] == '/') {
      /* Start C++ comment */
      cxx_comment = true;
      cp[0] = ' ';
      cp++;
      cp[0] = ' ';
    }
    else if (cp[0] == '/' && cp[1] == '*') {
      /* Start C comment */
      c_comment = true;
      cp[0] = ' ';
      cp++;
      cp[0] = ' ';
    }
    else if (cp[0] < ' ' || cp[0] > 128) {
      cp[0] = ' ';
    }
  }

  /* No need for leading '#' character. */
  const char *cpp_block_start = "ifdef __cplusplus";
  const char *cpp_block_start_alt = "if defined(__cplusplus)";
  const char *cpp_block_end = "endif";

  /* data from temp copy to maindata, remove irrelevant identifiers and double spaces */
  char *md = maindata;
  int newlen = 0;
  bool skip_until_closing_brace = false;
  int square_bracket_level = 0;
  int angle_bracket_level = 0;
  for (char *cp = temp; cp < temp + maindata_len; cp++) {
    if (cp[0] == '[') {
      square_bracket_level++;
    }
    else if (cp[0] == ']') {
      square_bracket_level--;
    }

    /* do not copy when: */
    if (cp[0] == ' ' && (square_bracket_level > 0)) {
      /* NOTE(@ideasman42): This is done to allow `member[C_STYLE_COMMENT 1024]`,
       * which is then read as `member[1024]`.
       * It's important to skip the spaces here,
       * otherwise the literal would be read as: `member[` and `1024]`. */
    }
    else if (cp[0] == ' ' && cp[1] == ' ') {
      /* pass */
    }
    else if (cp[-1] == '*' && cp[0] == ' ') {
      /* pointers with a space */
    } /* skip special keywords */
    else if (match_identifier(cp, "DNA_DEPRECATED")) {
      /* single values are skipped already, so decrement 1 less */
      cp += strlen("DNA_DEPRECATED") - 1;
    }
    else if (match_identifier(cp, "DNA_DEFINE_CXX_METHODS")) {
      /* single values are skipped already, so decrement 1 less */
      cp += strlen("DNA_DEFINE_CXX_METHODS") - 1;
      skip_until_closing_brace = true;
    }
    else if (skip_until_closing_brace) {
      if (cp[0] == ')') {
        skip_until_closing_brace = false;
      }
    }
    else if (match_preproc_prefix(cp, cpp_block_start) ||
             match_preproc_prefix(cp, cpp_block_start_alt))
    {
      char *end_ptr = match_preproc_strstr(cp, cpp_block_end);

      if (end_ptr == nullptr) {
        fprintf(stderr, "Error: '%s' block must end with '%s'\n", cpp_block_start, cpp_block_end);
      }
      else {
        const int skip_offset = end_ptr - cp + strlen(cpp_block_end);
        cp += skip_offset;
      }
    }
    /* Assume that type names ending with `T<` are templated types of legacy types. This is used to
     * treat `ListBaseT<Type>` as `ListBase` in SDNA. */
    else if (cp[0] == 'T' && cp[1] == '<') {
      angle_bracket_level = 1;
      cp++;
    }
    else if (angle_bracket_level >= 1) {
      if (cp[0] == '<') {
        angle_bracket_level++;
      }
      else if (cp[0] == '>') {
        angle_bracket_level--;
      }
    }
    else {
      md[0] = cp[0];
      md++;
      newlen++;
    }
  }

  BLI_assert(square_bracket_level == 0);

  MEM_delete(temp);
  return newlen;
}

static void *read_file_data(const char *filepath, int *r_len)
{
#ifdef WIN32
  FILE *fp = fopen(filepath, "rb");
#else
  FILE *fp = fopen(filepath, "r");
#endif
  void *data;

  if (!fp) {
    *r_len = -1;
    return nullptr;
  }

  fseek(fp, 0L, SEEK_END);
  *r_len = ftell(fp);
  fseek(fp, 0L, SEEK_SET);

  if (*r_len == -1) {
    fclose(fp);
    return nullptr;
  }

  data = MEM_new_uninitialized(*r_len, "read_file_data");
  if (!data) {
    *r_len = -1;
    fclose(fp);
    return nullptr;
  }

  if (fread(data, *r_len, 1, fp) != 1) {
    *r_len = -1;
    MEM_delete_void(data);
    fclose(fp);
    return nullptr;
  }

  fclose(fp);
  return data;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Member Name Canonicalization
 * \{ */

/**
 * Because of the weird way of tokenizing, we have to 'cast' function
 * pointers to ... (*f)(), whatever the original signature. In fact,
 * we add name and type at the same time... There are two special
 * cases, unfortunately. These are explicitly checked.
 *
 * \a r_extra_advance is set to the number of bytes past the `\0` terminator
 * of \a member_name that the caller should skip — needed for function pointers
 * like `(*func)(arg1 arg2)` whose internal space became a `\0` during
 * tokenization, so strlen only reaches the first arg.
 */
static bool canonicalize_member_name(const char *member_name,
                                     std::string &r_name,
                                     int &r_extra_advance)
{
  char buf[255]; /* stupid limit, change it :) */

  r_extra_advance = 0;

  if (member_name[0] == '(' && member_name[1] == '*') {
    /* We handle function pointer and special array cases here, e.g.
     * `void (*function)(...)` and `float (*array)[..]`. the array case
     * name is still converted to (array *)() though because it is that
     * way in old DNA too, and works correct with #DNA_struct_member_size. */
    int isfuncptr = (strchr(member_name + 1, '(')) != nullptr;

    DEBUG_PRINTF(3, "\t\t\t\t*** Function pointer or multidim array pointer found\n");
    /* function-pointer: transform the type (sometimes). */
    int i = 0;

    while (member_name[i] != ')') {
      buf[i] = member_name[i];
      i++;
    }

    /* Another number we need is the extra slen offset. This extra
     * offset is the overshoot after a space. If there is no
     * space, no overshoot should be calculated. */
    int j = i; /* j at first closing brace */

    DEBUG_PRINTF(3, "first brace after offset %d\n", i);

    j++; /* j beyond closing brace ? */
    while ((member_name[j] != 0) && (member_name[j] != ')')) {
      DEBUG_PRINTF(3, "seen %c (%d)\n", member_name[j], member_name[j]);
      j++;
    }
    DEBUG_PRINTF(3,
                 "seen %c (%d)\n"
                 "special after offset%d\n",
                 member_name[j],
                 member_name[j],
                 j);

    if (!isfuncptr) {
      /* multidimensional array pointer case */
      if (member_name[j] == 0) {
        DEBUG_PRINTF(3, "offsetting for multi-dimensional array pointer\n");
      }
      else {
        printf("Error during tokenizing multi-dimensional array pointer\n");
      }
    }
    else if (member_name[j] == 0) {
      DEBUG_PRINTF(3, "offsetting for space\n");
      /* get additional offset */
      int k = 0;
      while (member_name[j] != ')') {
        j++;
        k++;
      }
      DEBUG_PRINTF(3, "extra offset %d\n", k);
      r_extra_advance = k;
    }
    else if (member_name[j] == ')') {
      DEBUG_PRINTF(3, "offsetting for brace\n");
      /* don't get extra offset */
    }
    else {
      printf("Error during tokening function pointer argument list\n");
    }

    /*
     * Put `)(void)` at the end? Maybe `)()`. Should check this with
     * old `sdna`. Actually, sometimes `)()`, sometimes `)(void...)`
     * Alas.. such is the nature of brain-damage :(
     *
     * Sorted it out: always do )(), except for `headdraw` and
     * `windraw`, part of #ScrArea. This is important, because some
     * linkers will treat different fp's differently when called
     * !!! This has to do with interference in byte-alignment and
     * the way arguments are pushed on the stack.
     */
    buf[i] = 0;
    DEBUG_PRINTF(3, "Name before chomping: %s\n", buf);
    if ((strncmp(buf, "(*headdraw", 10) == 0) || strncmp(buf, "(*windraw", 9) == 0) {
      buf[i] = ')';
      buf[i + 1] = '(';
      buf[i + 2] = 'v';
      buf[i + 3] = 'o';
      buf[i + 4] = 'i';
      buf[i + 5] = 'd';
      buf[i + 6] = ')';
      buf[i + 7] = 0;
    }
    else {
      buf[i] = ')';
      buf[i + 1] = '(';
      buf[i + 2] = ')';
      buf[i + 3] = 0;
    }
    /* Now proceed with buf. */
    DEBUG_PRINTF(3, "\t\t\t\t\tProposing fp name %s\n", buf);
    r_name = buf;
  }
  else {
    /* normal field: old code */
    r_name = member_name;
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Main Parser
 * \{ */

bool parse_dna_header(const std::string &filepath, Vector<ParsedStruct> &r_structs)
{
  /* read include file, skip structs with a '#' before it.
   * store all data in temporal arrays.
   */
  int maindata_len;
  char *maindata = static_cast<char *>(read_file_data(filepath.c_str(), &maindata_len));
  char *md = maindata;
  if (maindata_len == -1) {
    fprintf(stderr, "Can't read file %s\n", filepath.c_str());
    return false;
  }

  maindata_len = preprocess_dna_header(maindata, maindata_len);
  char *mainend = maindata + maindata_len - 1;

  /* we look for '{' and then back to 'struct' */
  int count = 0;
  bool skip_struct = false;
  while (count < maindata_len) {

    /* code for skipping a struct: two hashes on 2 lines. (preprocess added a space) */
    if (md[0] == '#' && md[1] == ' ' && md[2] == '#') {
      skip_struct = true;
    }

    if (md[0] == '{') {
      md[0] = 0;
      if (skip_struct) {
        skip_struct = false;
      }
      else {
        if (md[-1] == ' ') {
          md[-1] = 0;
        }
        char *md1 = md - 2;
        while (*md1 != 32) {
          /* to beginning of word */
          md1--;
        }
        md1++;

        /* we've got a struct name when... */
        if (match_identifier(md1 - 7, "struct")) {

          if (!is_valid_type_name(md1, filepath.c_str())) {
            MEM_delete(maindata);
            return false;
          }

          ParsedStruct parsed_struct;
          parsed_struct.type_name = md1;

          DEBUG_PRINTF(1, "\t|\t|-- detected struct %s\n", parsed_struct.type_name.c_str());

          /* first lets make it all nice strings */
          md1 = md + 1;
          while (*md1 != '}') {
            if (md1 > mainend) {
              break;
            }

            /* Skip default value initializers. */
            if (*md1 == '=') {
              int braces_depth = 0;
              int brackets_depth = 0;
              while (true) {
                if (md1 > mainend) {
                  break;
                }

                if (*md1 == '{') {
                  braces_depth++;
                }
                else if (*md1 == '}') {
                  braces_depth--;
                }
                else if (*md1 == '(') {
                  brackets_depth++;
                }
                else if (*md1 == ')') {
                  brackets_depth--;
                }
                else if (braces_depth == 0 && brackets_depth == 0 && ELEM(*md1, ';', ',')) {
                  break;
                }

                *md1 = 0;
                md1++;
              }
            }

            if (ELEM(*md1, ',', ' ')) {
              *md1 = 0;
            }
            md1++;
          }

          /* read types and names until first character that is not '}' */
          md1 = md + 1;
          while (*md1 != '}') {
            if (md1 > mainend) {
              break;
            }

            /* skip when it says 'struct' or 'unsigned' or 'const' */
            if (*md1) {
              const char *md1_prev = md1;
              while (match_identifier_and_advance(&md1, "struct") ||
                     match_identifier_and_advance(&md1, "unsigned") ||
                     match_identifier_and_advance(&md1, "const"))
              {
                if (UNLIKELY(!ELEM(*md1, '\0', ' '))) {
                  /* This will happen with: `unsigned(*value)[3]` which isn't supported. */
                  fprintf(stderr,
                          "File '%s' contains non white space character "
                          "\"%c\" after identifier \"%s\"\n",
                          filepath.c_str(),
                          *md1,
                          md1_prev);
                  MEM_delete(maindata);
                  return false;
                }
                /* Skip ' ' or '\0'. */
                md1++;
              }

              /* we've got a type! */
              if (!is_valid_type_name(md1, filepath.c_str())) {
                MEM_delete(maindata);
                return false;
              }

              DEBUG_PRINTF(1, "\t|\t|\tfound type %s (", md1);

              const std::string type_name = md1;
              md1 += strlen(md1);

              /* read until ';' */
              while (*md1 != ';') {
                if (md1 > mainend) {
                  break;
                }

                if (*md1) {
                  /* We've got a name. slen needs
                   * correction for function
                   * pointers! */
                  int slen = int(strlen(md1));
                  if (md1[slen - 1] == ';') {
                    md1[slen - 1] = 0;

                    std::string member_name;
                    int extra_advance = 0;
                    if (!canonicalize_member_name(md1, member_name, extra_advance)) {
                      MEM_delete(maindata);
                      return false;
                    }
                    if (!is_valid_member_name(member_name.c_str(), filepath.c_str())) {
                      MEM_delete(maindata);
                      return false;
                    }
                    slen += extra_advance;

                    DEBUG_PRINTF(1, "%s |", member_name.c_str());

                    ParsedMember member;
                    member.type_name = type_name;
                    member.member_name = std::move(member_name);
                    parsed_struct.members.append(std::move(member));

                    md1 += slen;
                    break;
                  }

                  std::string member_name;
                  int extra_advance = 0;
                  if (!canonicalize_member_name(md1, member_name, extra_advance)) {
                    MEM_delete(maindata);
                    return false;
                  }
                  if (!is_valid_member_name(member_name.c_str(), filepath.c_str())) {
                    MEM_delete(maindata);
                    return false;
                  }
                  slen += extra_advance;

                  DEBUG_PRINTF(1, "%s ||", member_name.c_str());

                  ParsedMember member;
                  member.type_name = type_name;
                  member.member_name = std::move(member_name);
                  parsed_struct.members.append(std::move(member));

                  md1 += slen;
                }
                md1++;
              }

              DEBUG_PRINTF(1, ")\n");
            }
            md1++;
          }

          r_structs.append(std::move(parsed_struct));
        }
      }
    }
    count++;
    md++;
  }

  MEM_delete(maindata);

  return true;
}

/** \} */

}  // namespace dna
}  // namespace blender
