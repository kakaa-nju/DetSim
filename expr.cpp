#include "common.h"
#include "debug.h"
#include "guest.h"
#include "monitor.h"
#include <assert.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

enum
{
  OBJ,
  INT
};
typedef long op;
op nullop;

enum
{
  TK_NOTYPE = 254,
  TK_OR,
  TK_AND,
  TK_NOT,
  TK_EQ = 258,
  TK_PLUS = 260,
  TK_MINUS,
  TK_MUL = 264,
  TK_DIV,
  TK_NEG,
  TK_DEREF = 270,
  TK_MEMBER,
  TK_SYM,
  TK_DEC = 280,
  TK_VAR,
  TK_LP,
  TK_RP,
};

static struct rule
{
  const char* regex;
  int token_type;
} rules[] = {
    {" +", TK_NOTYPE}, // spaces, no precedence
    /* {"\\=", TK_ASSIGN}, // assignment */
    {"\\|\\|", TK_OR},
    {"&&", TK_AND},
    {"!", TK_NOT},
    {"==", TK_EQ},    // equal. last
    {"\\+", TK_PLUS}, // plus
    {"-", TK_MINUS},  // minus, and last go first (important)
    {"\\*", TK_MUL},
    {"/", TK_DIV},
    {"\\.", TK_MEMBER},
    {":", TK_SYM},
    {"0|[1-9][0-9]*", TK_DEC},
    {"[a-zA-Z_][a-zA-Z0-9_]*", TK_VAR},
    {"\\(", TK_LP},
    {"\\)", TK_RP},
};

#define ARRLEN(arr) (int)(sizeof(arr) / sizeof((arr)[0]))
#define NR_REGEX ARRLEN(rules)
static regex_t re[NR_REGEX] = {};

void init_regex()
{
  int i;
  char error_msg[128];
  int ret;

  for (i = 0; i < NR_REGEX; i++)
  {
    ret = regcomp(&re[i], rules[i].regex, REG_EXTENDED);
    if (ret != 0)
    {
      regerror(ret, &re[i], error_msg, 128);
      fprintf(stderr, "regex compilation failed: %s\n%s", error_msg,
              rules[i].regex);
      exit(EXIT_FAILURE);
    }
  }
}

typedef struct token
{
  int type;
  char str[32];
} Token;

static Token tokens[1024] __attribute__((used)) = {};
static int nr_token __attribute__((used)) = 0;

static bool make_token(const char* e)
{
  int position = 0;
  int i;
  regmatch_t pmatch;

  nr_token = 0;

  while (e[position] != '\0')
  {
    /* Try all rules one by one. */
    for (i = 0; i < NR_REGEX; i++)
    {
      if (regexec(&re[i], e + position, 1, &pmatch, 0) == 0 &&
          pmatch.rm_so == 0)
      {
        const char* substr_start = e + position;
        int substr_len = pmatch.rm_eo;

        LOG_TRACE("match rules[%d] = \"%s\" at position %d with len %d:"
                  "%.*s\n",
                  i, rules[i].regex, position, substr_len, substr_len,
                  substr_start);

        position += substr_len;

        switch (rules[i].token_type)
        {
          case TK_NOTYPE:
            break;
          case TK_MINUS:
            if (nr_token == 0 || (tokens[nr_token - 1].type != TK_DEC &&
                                  tokens[nr_token - 1].type != TK_VAR &&
                                  tokens[nr_token - 1].type != TK_RP))
              tokens[nr_token].type = TK_NEG;
            else
              tokens[nr_token].type = rules[i].token_type;
            nr_token++;
            break;
          default:
            tokens[nr_token].type = rules[i].token_type;
            snprintf(tokens[nr_token].str, substr_len + 1, "%s", substr_start);
            nr_token++;
            break;
        }
        break;
      }
    }

    if (i == NR_REGEX)
    {
      /* printf("no match at position %d\n%s\n%*.s^\n", position, e, position,
       * ""); */
      return false;
    }
  }
  return true;
}

bool check_parentheses(int p, int q)
{
  int cnt = 0;
  for (int i = p; i < q; i++)
  {
    if (tokens[i].type == TK_LP)
      cnt++;
    if (tokens[i].type == TK_RP)
      cnt--;
    if (cnt < 1)
      return false;
  }
  return cnt == 1 && tokens[p].type == TK_LP && tokens[q].type == TK_RP;
}

op eval(int p, int q, bool* success)
{
  if (p > q)
  {
    *success = false;
    return nullop;
  }
  else if (p == q)
  {
    assert(tokens[p].type == TK_DEC || tokens[p].type == TK_VAR);
    op ret;
    switch (tokens[p].type)
    {
      case TK_DEC:
        sscanf(tokens[p].str, "%ld", &ret);
        return ret;
      case TK_VAR:
        assert(0);
    }
  }
  else if (check_parentheses(p, q) == true)
  {
    return eval(p + 1, q - 1, success);
  }
  else
  {
    /* Main operator */
    int tk_type = 300;
    int n_operator = 0;
    int cnt = 0;
    for (int i = p; i <= q; i++)
    {
      if (tokens[i].type == TK_LP)
        cnt++;
      if (tokens[i].type == TK_RP)
        cnt--;
      if (cnt == 0 && ((tokens[i].type <= tk_type + 1 &&
                        !(tk_type == TK_NEG && tokens[i].type == TK_NEG))))
      {
        tk_type = tokens[i].type;
        n_operator = i;
      }
    }

    op ret;
    if (tk_type == TK_SYM)
    {
      assert(p == n_operator - 1);
      assert(tokens[p].type == TK_VAR);
      /* get node */
      int tracee_index = -1;
      sscanf(tokens[p].str, "tracee%d", &tracee_index);
      assert(tracee_index >= 0 && tracee_index < NP);

      tracee_state& ts = ptmc_state.dest_state.child[tracee_index];
      uintptr_t addr = get_var_addr(tokens[q].str);
      if (!addr)
        LOG_CRIT("Get &%s = NULL", tokens[q].str);
      void* mem = ts.read_snapshot_mem(addr, 8);

      ret = *(long*)mem;
      free(mem);
      LOG_DEBUG("get %s:%s = %ld at addr 0x%lx", tokens[p].str, tokens[q].str,
                ret, addr);

      return ret;
    }

    op val1, val2;
    if (tk_type != TK_NEG && tk_type != TK_NOT)
    {
      val1 = eval(p, n_operator - 1, success);
    }
    val2 = eval(n_operator + 1, q, success);
    if (!*success)
      return nullop;

    switch (tk_type)
    {
      case TK_PLUS:
        return val1 + val2;
      case TK_MINUS:
        return val1 - val2;
      case TK_MUL:
        return val1 * val2;
      case TK_DIV:
        return val1 / val2;
      case TK_NEG:
        return -val2;
      case TK_OR:
        return val1 || val2;
      case TK_AND:
        return val1 && val2;
      case TK_NOT:
        return !val1;
      case TK_EQ:
        return val1 == val2;
      default:
        *success = false;
        fprintf(stderr, "unrecognized operator %d", tk_type);
        exit(EXIT_FAILURE);
    }
  }
  return nullop;
}

long expr(const char* e, bool* success)
{
  *success = true;
  if (!make_token(e))
  {
    *success = false;
    return nullop;
  }
  return eval(0, nr_token - 1, success);
}
