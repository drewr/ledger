#include "valexpr.h"
#include "walk.h"
#include "error.h"
#include "datetime.h"
#include "debug.h"
#include "util.h"
#ifdef USE_BOOST_PYTHON
#include "py_eval.h"
#endif

namespace ledger {

std::auto_ptr<value_expr_t> amount_expr;
std::auto_ptr<value_expr_t> total_expr;

void value_expr_t::compute(value_t& result, const details_t& details) const
{
  switch (kind) {
  case CONSTANT_I:
    result = constant_i;
    break;
  case CONSTANT_T:
    result = long(constant_t);
    break;

  case CONSTANT_A:
    result = constant_a;
    break;

  case AMOUNT:
    if (details.xact) {
      if (transaction_has_xdata(*details.xact) &&
	  transaction_xdata_(*details.xact).dflags & TRANSACTION_COMPOSITE)
	result = transaction_xdata_(*details.xact).composite_amount;
      else
	result = details.xact->amount;
    }
    else if (details.account && account_has_xdata(*details.account)) {
      result = account_xdata(*details.account).value;
    }
    else {
      result = 0L;
    }
    break;

  case COST:
    if (details.xact) {
      bool set = false;
      if (transaction_has_xdata(*details.xact)) {
	transaction_xdata_t& xdata(transaction_xdata_(*details.xact));
	if (xdata.dflags & TRANSACTION_COMPOSITE) {
	  if (xdata.composite_amount.type == value_t::BALANCE_PAIR &&
	      ((balance_pair_t *) xdata.composite_amount.data)->cost)
	    result = *((balance_pair_t *) xdata.composite_amount.data)->cost;
	  else
	    result = xdata.composite_amount;
	  set = true;
	}
      }

      if (! set) {
	if (details.xact->cost)
	  result = *details.xact->cost;
	else
	  result = details.xact->amount;
      }
    }
    else if (details.account && account_has_xdata(*details.account)) {
      result = account_xdata(*details.account).value.cost();
    }
    else {
      result = 0L;
    }
    break;

  case TOTAL:
    if (details.xact && transaction_has_xdata(*details.xact))
      result = transaction_xdata_(*details.xact).total;
    else if (details.account && account_has_xdata(*details.account))
      result = account_xdata(*details.account).total;
    else
      result = 0L;
    break;
  case COST_TOTAL:
    if (details.xact && transaction_has_xdata(*details.xact))
      result = transaction_xdata_(*details.xact).total.cost();
    else if (details.account && account_has_xdata(*details.account))
      result = account_xdata(*details.account).total.cost();
    else
      result = 0L;
    break;

  case VALUE_EXPR:
    if (amount_expr.get())
      amount_expr->compute(result, details);
    else
      result = 0L;
    break;
  case TOTAL_EXPR:
    if (total_expr.get())
      total_expr->compute(result, details);
    else
      result = 0L;
    break;

  case DATE:
    if (details.xact && transaction_has_xdata(*details.xact) &&
	transaction_xdata_(*details.xact).date)
      result = long(transaction_xdata_(*details.xact).date);
    else if (details.entry)
      result = long(details.entry->date);
    else
      result = long(now);
    break;

  case CLEARED:
    if (details.entry)
      result = details.entry->state == entry_t::CLEARED;
    else
      result = false;
    break;

  case REAL:
    if (details.xact)
      result = ! (details.xact->flags & TRANSACTION_VIRTUAL);
    else
      result = true;
    break;

  case ACTUAL:
    if (details.xact)
      result = ! (details.xact->flags & TRANSACTION_AUTO);
    else
      result = true;
    break;

  case INDEX:
    if (details.xact && transaction_has_xdata(*details.xact))
      result = long(transaction_xdata_(*details.xact).index + 1);
    else if (details.account && account_has_xdata(*details.account))
      result = long(account_xdata(*details.account).count);
    else
      result = 0L;
    break;

  case COUNT:
    if (details.xact && transaction_has_xdata(*details.xact))
      result = long(transaction_xdata_(*details.xact).index + 1);
    else if (details.account && account_has_xdata(*details.account))
      result = long(account_xdata(*details.account).total_count);
    else
      result = 0L;
    break;

  case DEPTH:
    if (details.account)
      result = long(details.account->depth);
    else
      result = 0L;
    break;

  case F_ARITH_MEAN:
    if (details.xact && transaction_has_xdata(*details.xact)) {
      assert(left);
      left->compute(result, details);
      result /= amount_t(long(transaction_xdata_(*details.xact).index + 1));
    }
    else if (details.account && account_has_xdata(*details.account) &&
	     account_xdata(*details.account).total_count) {
      assert(left);
      left->compute(result, details);
      result /= amount_t(long(account_xdata(*details.account).total_count));
    }
    else {
      result = 0L;
    }
    break;

  case F_PARENT:
    if (details.account && details.account->parent)
      left->compute(result, details_t(*details.account->parent));
    break;

  case F_NEG:
    assert(left);
    left->compute(result, details);
    result.negate();
    break;

  case F_ABS:
    assert(left);
    left->compute(result, details);
    result.abs();
    break;

  case F_STRIP: {
    assert(left);
    left->compute(result, details);

    balance_t * bal = NULL;
    switch (result.type) {
    case value_t::BALANCE_PAIR:
      bal = &((balance_pair_t *) result.data)->quantity;
      // fall through...

    case value_t::BALANCE:
      if (! bal)
	bal = (balance_t *) result.data;

      if (bal->amounts.size() < 2) {
	result.cast(value_t::AMOUNT);
      } else {
	value_t temp;
	for (amounts_map::const_iterator i = bal->amounts.begin();
	     i != bal->amounts.end();
	     i++) {
	  amount_t x = (*i).second;
	  x.clear_commodity();
	  temp += x;
	}
	result = temp;
	assert(temp.type == value_t::AMOUNT);
      }
      // fall through...

    case value_t::AMOUNT:
      ((amount_t *) result.data)->clear_commodity();
      break;

    default:
      break;
    }
    break;
  }

  case F_PAYEE_MASK:
    assert(mask);
    if (details.entry)
      result = mask->match(details.entry->payee);
    else
      result = false;
    break;

  case F_ACCOUNT_MASK:
    assert(mask);
    if (details.account)
      result = mask->match(details.account->fullname());
    else
      result = false;
    break;

  case F_SHORT_ACCOUNT_MASK:
    assert(mask);
    if (details.account)
      result = mask->match(details.account->name);
    else
      result = false;
    break;

  case F_VALUE: {
    assert(left);
    left->compute(result, details);

    std::time_t moment = now;
    if (right) {
      switch (right->kind) {
      case DATE:
	if (details.xact && transaction_has_xdata(*details.xact) &&
	    transaction_xdata_(*details.xact).date)
	  moment = transaction_xdata_(*details.xact).date;
	else if (details.entry)
	  moment = details.entry->date;
	break;
      case CONSTANT_T:
	moment = right->constant_t;
	break;
      default:
	throw compute_error("Invalid date passed to P(value,date)");
      }
    }

    result = result.value(moment);
    break;
  }

  case F_INTERP_FUNC: {
#ifdef USE_BOOST_PYTHON
    if (! python_call(constant_s, right, details, result))
      result = 0L;
#else
    result = 0L;
#endif
    break;
  }

  case O_NOT:
    left->compute(result, details);
    result.negate();
    break;

  case O_QUES: {
    assert(left);
    assert(right);
    assert(right->kind == O_COL);
    left->compute(result, details);
    if (result)
      right->left->compute(result, details);
    else
      right->right->compute(result, details);
    break;
  }

  case O_AND:
    assert(left);
    assert(right);
    left->compute(result, details);
    if (result)
      right->compute(result, details);
    break;

  case O_OR:
    assert(left);
    assert(right);
    left->compute(result, details);
    if (! result)
      right->compute(result, details);
    break;

  case O_EQ:
  case O_LT:
  case O_LTE:
  case O_GT:
  case O_GTE: {
    assert(left);
    assert(right);
    value_t temp;
    left->compute(temp, details);
    right->compute(result, details);
    switch (kind) {
    case O_EQ:  result = temp == result; break;
    case O_LT:  result = temp <  result; break;
    case O_LTE: result = temp <= result; break;
    case O_GT:  result = temp >  result; break;
    case O_GTE: result = temp >= result; break;
    default: assert(0); break;
    }
    break;
  }

  case O_ADD:
  case O_SUB:
  case O_MUL:
  case O_DIV: {
    assert(left);
    assert(right);
    value_t temp;
    right->compute(temp, details);
    left->compute(result, details);
    switch (kind) {
    case O_ADD: result += temp; break;
    case O_SUB: result -= temp; break;
    case O_MUL: result *= temp; break;
    case O_DIV: result /= temp; break;
    default: assert(0); break;
    }
    break;
  }

  case LAST:
  default:
    assert(0);
    break;
  }
}

static inline void unexpected(char c, char wanted = '\0') {
  if ((unsigned char) c == 0xff) {
    if (wanted)
      throw value_expr_error(std::string("Missing '") + wanted + "'");
    else
      throw value_expr_error("Unexpected end");
  } else {
    if (wanted)
      throw value_expr_error(std::string("Invalid char '") + c +
			     "' (wanted '" + wanted + "')");
    else
      throw value_expr_error(std::string("Invalid char '") + c + "'");
  }
}

value_expr_t * parse_value_term(std::istream& in);

inline value_expr_t * parse_value_term(const char * p) {
  std::istringstream stream(p);
  return parse_value_term(stream);
}

value_expr_t * parse_value_term(std::istream& in)
{
  std::auto_ptr<value_expr_t> node;

  char buf[256];
  char c = peek_next_nonws(in);
  if (std::isdigit(c)) {
    READ_INTO(in, buf, 255, c, std::isdigit(c));

    node.reset(new value_expr_t(value_expr_t::CONSTANT_I));
    node->constant_i = std::atol(buf);
    return node.release();
  }
  else if (c == '{') {
    in.get(c);
    READ_INTO(in, buf, 255, c, c != '}');
    if (c == '}')
      in.get(c);
    else
      unexpected(c, '}');

    node.reset(new value_expr_t(value_expr_t::CONSTANT_A));
    node->constant_a.parse(buf);
    return node.release();
  }

  in.get(c);
  switch (c) {
  // Basic terms
  case 'm':
    node.reset(new value_expr_t(value_expr_t::CONSTANT_T));
    node->constant_t = now;
    break;

  case 'a': node.reset(new value_expr_t(value_expr_t::AMOUNT)); break;
  case 'b': node.reset(new value_expr_t(value_expr_t::COST)); break;
  case 'd': node.reset(new value_expr_t(value_expr_t::DATE)); break;
  case 'X': node.reset(new value_expr_t(value_expr_t::CLEARED)); break;
  case 'R': node.reset(new value_expr_t(value_expr_t::REAL)); break;
  case 'L': node.reset(new value_expr_t(value_expr_t::ACTUAL)); break;
  case 'n': node.reset(new value_expr_t(value_expr_t::INDEX)); break;
  case 'N': node.reset(new value_expr_t(value_expr_t::COUNT)); break;
  case 'l': node.reset(new value_expr_t(value_expr_t::DEPTH)); break;
  case 'O': node.reset(new value_expr_t(value_expr_t::TOTAL)); break;
  case 'B': node.reset(new value_expr_t(value_expr_t::COST_TOTAL)); break;

  // Relating to format_t
  case 't': node.reset(new value_expr_t(value_expr_t::VALUE_EXPR)); break;
  case 'T': node.reset(new value_expr_t(value_expr_t::TOTAL_EXPR)); break;

  // Compound terms
  case 'v': node.reset(parse_value_expr("P(a,d)")); break;
  case 'V': node.reset(parse_value_term("P(O,d)")); break;
  case 'g': node.reset(parse_value_expr("v-b")); break;
  case 'G': node.reset(parse_value_expr("V-B")); break;

  // Functions
  case '^':
    node.reset(new value_expr_t(value_expr_t::F_PARENT));
    node->left = parse_value_term(in);
    break;

  case '-':
    node.reset(new value_expr_t(value_expr_t::F_NEG));
    node->left = parse_value_term(in);
    break;

  case 'U':
    node.reset(new value_expr_t(value_expr_t::F_ABS));
    node->left = parse_value_term(in);
    break;

  case 'S':
    node.reset(new value_expr_t(value_expr_t::F_STRIP));
    node->left = parse_value_term(in);
    break;

  case 'A':
    node.reset(new value_expr_t(value_expr_t::F_ARITH_MEAN));
    node->left = parse_value_term(in);
    break;

  case 'P':
    node.reset(new value_expr_t(value_expr_t::F_VALUE));
    if (peek_next_nonws(in) == '(') {
      in.get(c);
      node->left = parse_value_expr(in, true);
      if (peek_next_nonws(in) == ',') {
	in.get(c);
	node->right = parse_value_expr(in, true);
      }
      in.get(c);
      if (c != ')')
	unexpected(c, ')');
    } else {
      node->left = parse_value_term(in);
    }
    break;

  // Other
  case '/': {
    bool payee_mask	    = false;
    bool short_account_mask = false;

    c = peek_next_nonws(in);
    if (c == '/') {
      in.get(c);
      c = in.peek();
      if (c == '/') {
	in.get(c);
	c = in.peek();
	short_account_mask = true;
      } else {
	payee_mask = true;
      }
    }

    READ_INTO(in, buf, 255, c, c != '/');
    if (c != '/')
      unexpected(c, '/');

    in.get(c);
    node.reset(new value_expr_t(short_account_mask ?
				value_expr_t::F_SHORT_ACCOUNT_MASK :
				(payee_mask ? value_expr_t::F_PAYEE_MASK :
				 value_expr_t::F_ACCOUNT_MASK)));
    node->mask = new mask_t(buf);
    break;
  }

  case '@': {
    READ_INTO(in, buf, 255, c, c != '(');
    if (c != '(')
      unexpected(c, '(');

    node.reset(new value_expr_t(value_expr_t::F_INTERP_FUNC));
    node->constant_s = buf;

    in.get(c);
    if (peek_next_nonws(in) == ')') {
      in.get(c);
    } else {
      node->right = new value_expr_t(value_expr_t::O_ARG);
      value_expr_t * cur = node->right;
      cur->left = parse_value_expr(in, true);
      in.get(c);
      while (! in.eof() && c == ',') {
	cur->right = new value_expr_t(value_expr_t::O_ARG);
	cur = cur->right;
	cur->left = parse_value_expr(in, true);
	in.get(c);
      }
      if (c != ')')
	unexpected(c, ')');
    }
    break;
  }

  case '(':
    node.reset(parse_value_expr(in, true));
    in.get(c);
    if (c != ')')
      unexpected(c, ')');
    break;

  case '[': {
    READ_INTO(in, buf, 255, c, c != ']');
    if (c != ']')
      unexpected(c, ']');
    in.get(c);

    node.reset(new value_expr_t(value_expr_t::CONSTANT_T));

    interval_t timespan(buf);
    node->constant_t = timespan.first();
    break;
  }

  default:
    in.unget();
    break;
  }

  return node.release();
}

value_expr_t * parse_mul_expr(std::istream& in)
{
  std::auto_ptr<value_expr_t> node(parse_value_term(in));

  if (node.get() && ! in.eof()) {
    char c = peek_next_nonws(in);
    while (c == '*' || c == '/') {
      in.get(c);
      switch (c) {
      case '*': {
	std::auto_ptr<value_expr_t> prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_MUL));
	node->left  = prev.release();
	node->right = parse_value_term(in);
	break;
      }

      case '/': {
	std::auto_ptr<value_expr_t> prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_DIV));
	node->left  = prev.release();
	node->right = parse_value_term(in);
	break;
      }
      }
      c = peek_next_nonws(in);
    }
  }

  return node.release();
}

value_expr_t * parse_add_expr(std::istream& in)
{
  std::auto_ptr<value_expr_t> node(parse_mul_expr(in));

  if (node.get() && ! in.eof()) {
    char c = peek_next_nonws(in);
    while (c == '+' || c == '-') {
      in.get(c);
      switch (c) {
      case '+': {
	std::auto_ptr<value_expr_t> prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_ADD));
	node->left  = prev.release();
	node->right = parse_mul_expr(in);
	break;
      }

      case '-': {
	std::auto_ptr<value_expr_t> prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_SUB));
	node->left  = prev.release();
	node->right = parse_mul_expr(in);
	break;
      }
      }
      c = peek_next_nonws(in);
    }
  }

  return node.release();
}

value_expr_t * parse_logic_expr(std::istream& in)
{
  std::auto_ptr<value_expr_t> node;

  if (peek_next_nonws(in) == '!') {
    char c;
    in.get(c);
    node.reset(new value_expr_t(value_expr_t::O_NOT));
    node->left = parse_logic_expr(in);
    return node.release();
  }

  node.reset(parse_add_expr(in));

  if (node.get() && ! in.eof()) {
    char c = peek_next_nonws(in);
    if (c == '=' || c == '<' || c == '>') {
      in.get(c);
      switch (c) {
      case '=': {
	std::auto_ptr<value_expr_t> prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_EQ));
	node->left  = prev.release();
	node->right = parse_add_expr(in);
	break;
      }

      case '<': {
	std::auto_ptr<value_expr_t> prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_LT));
	if (peek_next_nonws(in) == '=') {
	  in.get(c);
	  node->kind = value_expr_t::O_LTE;
	}
	node->left  = prev.release();
	node->right = parse_add_expr(in);
	break;
      }

      case '>': {
	std::auto_ptr<value_expr_t> prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_GT));
	if (peek_next_nonws(in) == '=') {
	  in.get(c);
	  node->kind = value_expr_t::O_GTE;
	}
	node->left  = prev.release();
	node->right = parse_add_expr(in);
	break;
      }

      default:
	if (! in.eof())
	  unexpected(c);
	break;
      }
    }
  }

  return node.release();
}

value_expr_t * parse_value_expr(std::istream& in, const bool partial)
{
  std::auto_ptr<value_expr_t> node(parse_logic_expr(in));

  if (node.get() && ! in.eof()) {
    char c = peek_next_nonws(in);
    while (c == '&' || c == '|' || c == '?') {
      in.get(c);
      switch (c) {
      case '&': {
	std::auto_ptr<value_expr_t> prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_AND));
	node->left  = prev.release();
	node->right = parse_logic_expr(in);
	break;
      }

      case '|': {
	std::auto_ptr<value_expr_t> prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_OR));
	node->left  = prev.release();
	node->right = parse_logic_expr(in);
	break;
      }

      case '?': {
	std::auto_ptr<value_expr_t> prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_QUES));
	node->left  = prev.release();
	value_expr_t * choices;
	node->right = choices = new value_expr_t(value_expr_t::O_COL);
	choices->left = parse_logic_expr(in);
	c = peek_next_nonws(in);
	if (c != ':')
	  unexpected(c, ':');
	in.get(c);
	choices->right = parse_logic_expr(in);
	break;
      }

      default:
	if (! in.eof())
	  unexpected(c);
	break;
      }
      c = peek_next_nonws(in);
    }
  }

  char c;
  if (! node.get()) {
    in.get(c);
    if (in.eof())
      throw value_expr_error(std::string("Failed to parse value expression"));
    else
      unexpected(c);
  } else if (! partial) {
    in.get(c);
    if (! in.eof())
      unexpected(c);
    else
      in.unget();
  }

  return node.release();
}

#ifdef DEBUG_ENABLED

void dump_value_expr(std::ostream& out, const value_expr_t * node)
{
  switch (node->kind) {
  case value_expr_t::CONSTANT_I:
    out << "UINT[" << node->constant_i << ']';
    break;
  case value_expr_t::CONSTANT_T:
    out << "DATE/TIME[" << node->constant_t << ']';
    break;
  case value_expr_t::CONSTANT_A:
    out << "CONST[" << node->constant_a << ']';
    break;

  case value_expr_t::AMOUNT:	   out << "AMOUNT"; break;
  case value_expr_t::COST:	   out << "COST"; break;
  case value_expr_t::DATE:	   out << "DATE"; break;
  case value_expr_t::CLEARED:	   out << "CLEARED"; break;
  case value_expr_t::REAL:	   out << "REAL"; break;
  case value_expr_t::ACTUAL:	   out << "ACTUAL"; break;
  case value_expr_t::INDEX:	   out << "INDEX"; break;
  case value_expr_t::COUNT:	   out << "COUNT"; break;
  case value_expr_t::DEPTH:	   out << "DEPTH"; break;
  case value_expr_t::TOTAL:        out << "TOTAL"; break;
  case value_expr_t::COST_TOTAL:   out << "COST_TOTAL"; break;

  case value_expr_t::F_ARITH_MEAN:
    out << "MEAN(";
    dump_value_expr(out, node->left);
    out << ')';
    break;

  case value_expr_t::F_NEG:
    out << "ABS(";
    dump_value_expr(out, node->left);
    out << ')';
    break;

  case value_expr_t::F_ABS:
    out << "ABS(";
    dump_value_expr(out, node->left);
    out << ')';
    break;

  case value_expr_t::F_STRIP:
    out << "STRIP(";
    dump_value_expr(out, node->left);
    out << ')';
    break;

  case value_expr_t::F_PAYEE_MASK:
    assert(node->mask);
    out << "P_MASK(" << node->mask->pattern << ')';
    break;

  case value_expr_t::F_ACCOUNT_MASK:
    assert(node->mask);
    out << "A_MASK(" << node->mask->pattern << ')';
    break;

  case value_expr_t::F_SHORT_ACCOUNT_MASK:
    assert(node->mask);
    out << "A_SMASK(" << node->mask->pattern << ')';
    break;

  case value_expr_t::F_VALUE:
    out << "VALUE(";
    dump_value_expr(out, node->left);
    if (node->right) {
      out << ", ";
      dump_value_expr(out, node->right);
    }
    out << ')';
    break;

  case value_expr_t::F_INTERP_FUNC:
    out << "F_INTERP[" << node->constant_s << "](";
    dump_value_expr(out, node->right);
    out << ')';
    break;

  case value_expr_t::O_NOT:
    out << '!';
    dump_value_expr(out, node->left);
    break;

  case value_expr_t::O_ARG:
    dump_value_expr(out, node->left);
    if (node->right) {
      out << ',';
      dump_value_expr(out, node->right);
    }
    break;

  case value_expr_t::O_QUES:
    dump_value_expr(out, node->left);
    out << '?';
    dump_value_expr(out, node->right->left);
    out << ':';
    dump_value_expr(out, node->right->right);
    break;

  case value_expr_t::O_AND:
  case value_expr_t::O_OR:
    out << '(';
    dump_value_expr(out, node->left);
    switch (node->kind) {
    case value_expr_t::O_AND: out << " & "; break;
    case value_expr_t::O_OR:  out << " | "; break;
    default: assert(0); break;
    }
    dump_value_expr(out, node->right);
    out << ')';
    break;

  case value_expr_t::O_EQ:
  case value_expr_t::O_LT:
  case value_expr_t::O_LTE:
  case value_expr_t::O_GT:
  case value_expr_t::O_GTE:
    out << '(';
    dump_value_expr(out, node->left);
    switch (node->kind) {
    case value_expr_t::O_EQ:  out << '='; break;
    case value_expr_t::O_LT:  out << '<'; break;
    case value_expr_t::O_LTE: out << "<="; break;
    case value_expr_t::O_GT:  out << '>'; break;
    case value_expr_t::O_GTE: out << ">="; break;
    default: assert(0); break;
    }
    dump_value_expr(out, node->right);
    out << ')';
    break;

  case value_expr_t::O_ADD:
  case value_expr_t::O_SUB:
  case value_expr_t::O_MUL:
  case value_expr_t::O_DIV:
    out << '(';
    dump_value_expr(out, node->left);
    switch (node->kind) {
    case value_expr_t::O_ADD: out << '+'; break;
    case value_expr_t::O_SUB: out << '-'; break;
    case value_expr_t::O_MUL: out << '*'; break;
    case value_expr_t::O_DIV: out << '/'; break;
    default: assert(0); break;
    }
    dump_value_expr(out, node->right);
    out << ')';
    break;

  case value_expr_t::LAST:
  default:
    assert(0);
    break;
  }
}

#endif // DEBUG_ENABLED

} // namespace ledger

#ifdef USE_BOOST_PYTHON

#include <boost/python.hpp>

using namespace boost::python;
using namespace ledger;

value_t py_compute_1(value_expr_t& value_expr, const details_t& item)
{
  value_t result;
  value_expr.compute(result, item);
  return result;
}

template <typename T>
value_t py_compute(value_expr_t& value_expr, const T& item)
{
  value_t result;
  value_expr.compute(result, details_t(item));
  return result;
}

value_expr_t * py_parse_value_expr_1(const std::string& str)
{
  return parse_value_expr(str);
}

value_expr_t * py_parse_value_expr_2(const std::string& str, const bool partial)
{
  return parse_value_expr(str, partial);
}

#define EXC_TRANSLATOR(type)				\
  void exc_translate_ ## type(const type& err) {	\
    PyErr_SetString(PyExc_RuntimeError, err.what());	\
  }

EXC_TRANSLATOR(value_expr_error)
EXC_TRANSLATOR(compute_error)
EXC_TRANSLATOR(mask_error)

void export_valexpr()
{
  class_< details_t > ("Details", init<const entry_t&>())
    .def(init<const transaction_t&>())
    .def(init<const account_t&>())
    .add_property("entry",
		  make_getter(&details_t::entry,
			      return_value_policy<reference_existing_object>()))
    .add_property("xact",
		  make_getter(&details_t::xact,
			      return_value_policy<reference_existing_object>()))
    .add_property("account",
		  make_getter(&details_t::account,
			      return_value_policy<reference_existing_object>()))
    ;

  class_< value_expr_t > ("ValueExpr", init<value_expr_t::kind_t>())
    .def("compute", py_compute_1)
    .def("compute", py_compute<account_t>)
    .def("compute", py_compute<entry_t>)
    .def("compute", py_compute<transaction_t>)
    ;

  def("parse_value_expr", py_parse_value_expr_1,
      return_value_policy<manage_new_object>());
  def("parse_value_expr", py_parse_value_expr_2,
      return_value_policy<manage_new_object>());

  class_< item_predicate<transaction_t> >
    ("TransactionPredicate", init<std::string>())
    .def("__call__", &item_predicate<transaction_t>::operator())
    ;

  class_< item_predicate<account_t> >
    ("AccountPredicate", init<std::string>())
    .def("__call__", &item_predicate<account_t>::operator())
    ;

#define EXC_TRANSLATE(type)					\
  register_exception_translator<type>(&exc_translate_ ## type);

  EXC_TRANSLATE(value_expr_error);
  EXC_TRANSLATE(compute_error);
  EXC_TRANSLATE(mask_error);
}

#endif // USE_BOOST_PYTHON

#ifdef TEST

int main(int argc, char *argv[])
{
  ledger::dump_value_expr(std::cout, ledger::parse_value_expr(argv[1]));
  std::cout << std::endl;
}

#endif // TEST