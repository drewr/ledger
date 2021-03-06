/*
 * Copyright (c) 2003-2009, John Wiegley.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * - Neither the name of New Artisans LLC nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <system.hh>

#include "filters.h"
#include "iterators.h"
#include "journal.h"
#include "report.h"
#include "compare.h"

namespace ledger {

pass_down_posts::pass_down_posts(post_handler_ptr handler,
				 posts_iterator& iter)
  : item_handler<post_t>(handler)
{
  TRACE_CTOR(pass_down_posts, "post_handler_ptr, posts_iterator");

  for (post_t * post = iter(); post; post = iter()) {
    try {
      item_handler<post_t>::operator()(*post);
    }
    catch (const std::exception& err) {
      add_error_context(item_context(*post, _("While handling posting")));
      throw;
    }
  }

  item_handler<post_t>::flush();
}

void truncate_xacts::flush()
{
  if (! posts.size())
    return;

  xact_t * xact = (*posts.begin())->xact;

  int l = 0;
  foreach (post_t * post, posts)
    if (xact != post->xact) {
      l++;
      xact = post->xact;
    }
  l++;

  xact = (*posts.begin())->xact;

  int i = 0;
  foreach (post_t * post, posts) {
    if (xact != post->xact) {
      xact = post->xact;
      i++;
    }

    bool print = false;
    if (head_count) {
      if (head_count > 0 && i < head_count)
	print = true;
      else if (head_count < 0 && i >= - head_count)
	print = true;
    }

    if (! print && tail_count) {
      if (tail_count > 0 && l - i <= tail_count)
	print = true;
      else if (tail_count < 0 && l - i > - tail_count)
	print = true;
    }

    if (print)
      item_handler<post_t>::operator()(*post);
  }
  posts.clear();

  item_handler<post_t>::flush();
}

void truncate_xacts::operator()(post_t& post)
{
  if (last_xact != post.xact) {
    if (last_xact)
      xacts_seen++;
    last_xact = post.xact;
  }

  if (tail_count == 0 && head_count > 0 &&
      static_cast<int>(xacts_seen) >= head_count)
    return;

  posts.push_back(&post);
}

void sort_posts::post_accumulated_posts()
{
  std::stable_sort(posts.begin(), posts.end(),
		   compare_items<post_t>(sort_order));

  foreach (post_t * post, posts) {
    post->xdata().drop_flags(POST_EXT_SORT_CALC);
    item_handler<post_t>::operator()(*post);
  }

  posts.clear();
}

namespace {
  string to_hex(uint_least32_t * message_digest)
  {
    std::ostringstream buf;

    for(int i = 0; i < 5 ; i++) {
      buf.width(8);
      buf.fill('0');
      buf << std::hex << message_digest[i];
    }
    return buf.str();
  }
}

void anonymize_posts::operator()(post_t& post)
{
  SHA1		 sha;
  uint_least32_t message_digest[5];
  bool		 copy_xact_details = false;

  if (last_xact != post.xact) {
    xact_temps.push_back(*post.xact);
    last_xact = post.xact;
    copy_xact_details = true;
  }
  xact_t& xact = xact_temps.back();

  if (copy_xact_details) {
    xact.copy_details(*post.xact);

    sha.Reset();
    sha << post.xact->payee.c_str();
    sha.Result(message_digest);

    xact.payee = to_hex(message_digest);
    xact.note  = none;
  }

  post_temps.push_back(post);
  post_t& temp = post_temps.back();
  temp.xact = &xact;

  sha.Reset();
  sha << post.account->fullname().c_str();
  sha.Result(message_digest);

  temp.copy_details(post);

  temp.account = post.xact->journal->find_account(to_hex(message_digest));
  temp.note    = none;
  temp.add_flags(ITEM_TEMP);

  xact.add_post(&temp);

  (*handler)(temp);
}

void calc_posts::operator()(post_t& post)
{
  post_t::xdata_t& xdata(post.xdata());

  if (last_post) {
    assert(last_post->has_xdata());
    if (! account_wise)
      xdata.total = last_post->xdata().total;
    xdata.count = last_post->xdata().count + 1;
  } else {
    xdata.count = 1;
  }

  post.add_to_value(xdata.visited_value, amount_expr);
  xdata.add_flags(POST_EXT_VISITED);

  account_t * acct = post.reported_account();
  acct->xdata().add_flags(ACCOUNT_EXT_VISITED);

  if (! account_wise)
    add_or_set_value(xdata.total, xdata.visited_value);

  item_handler<post_t>::operator()(post);

  last_post = &post;
}

namespace {
  typedef function<void (post_t *)> post_functor_t;

  void handle_value(const value_t&	            value,
		    account_t *		            account,
		    xact_t *		            xact,
		    std::list<post_t>&              temps,
		    item_handler<post_t>&           handler,
		    const date_t&                   date          = date_t(),
		    const value_t&                  total         = value_t(),
		    const bool                      direct_amount = false,
		    const optional<post_functor_t>& functor       = none)
  {
    temps.push_back(post_t(account));
    post_t& post(temps.back());
    post.xact = xact;
    post.add_flags(ITEM_TEMP);
    xact->add_post(&post);

    // If the account for this post is all virtual, then report the post as
    // such.  This allows subtotal reports to show "(Account)" for accounts
    // that contain only virtual posts.
    if (account && account->has_xdata() &&
	account->xdata().has_flags(ACCOUNT_EXT_AUTO_VIRTUALIZE)) {
      if (! account->xdata().has_flags(ACCOUNT_EXT_HAS_NON_VIRTUALS)) {
	post.add_flags(POST_VIRTUAL);
	if (! account->xdata().has_flags(ACCOUNT_EXT_HAS_UNB_VIRTUALS))
	  post.add_flags(POST_MUST_BALANCE);
      }
    }

    post_t::xdata_t& xdata(post.xdata());

    if (is_valid(date))
      xdata.date = date;

    value_t temp(value);

    switch (value.type()) {
    case value_t::BOOLEAN:
    case value_t::INTEGER:
      temp.in_place_cast(value_t::AMOUNT);
      // fall through...

    case value_t::AMOUNT:
      post.amount = temp.as_amount();
      break;

    case value_t::BALANCE:
    case value_t::SEQUENCE:
      xdata.compound_value = temp;
      xdata.add_flags(POST_EXT_COMPOUND);
      break;

    case value_t::DATETIME:
    case value_t::DATE:
    default:
      assert(false);
      break;
    }

    if (! total.is_null())
      xdata.total = total;

    if (direct_amount)
      xdata.add_flags(POST_EXT_DIRECT_AMT);

    if (functor)
      (*functor)(&post);

    DEBUG("filter.changed_value.rounding", "post.amount = " << post.amount);

    handler(post);
  }
}

void collapse_posts::report_subtotal()
{
  if (! count)
    return;

  std::size_t displayed_count = 0;
  foreach (post_t * post, component_posts) {
    if (only_predicate(*post) && display_predicate(*post))
      displayed_count++;
  }  

  if (displayed_count == 1) {
    item_handler<post_t>::operator()(*last_post);
  }
  else if (only_collapse_if_zero && ! subtotal.is_zero()) {
    foreach (post_t * post, component_posts)
      item_handler<post_t>::operator()(*post);
  }
  else {
    date_t earliest_date;

    foreach (post_t * post, component_posts) {
      date_t reported = post->date();
      if (! is_valid(earliest_date) ||
	  reported < earliest_date)
	earliest_date = reported;
    }

    xact_temps.push_back(xact_t());
    xact_t& xact = xact_temps.back();
    xact.payee	   = last_xact->payee;
    xact._date	   = (is_valid(earliest_date) ?
		      earliest_date : last_xact->_date);
    DEBUG("filter.collapse", "Pseudo-xact date = " << *xact._date);

    handle_value(subtotal, &totals_account, &xact, post_temps, *handler);
  }

  component_posts.clear();

  last_xact = NULL;
  last_post  = NULL;
  subtotal   = 0L;
  count      = 0;
}

void collapse_posts::operator()(post_t& post)
{
  // If we've reached a new xact, report on the subtotal
  // accumulated thus far.

  if (last_xact != post.xact && count > 0)
    report_subtotal();

  post.add_to_value(subtotal, amount_expr);
  count++;

  component_posts.push_back(&post);

  last_xact = post.xact;
  last_post  = &post;
}

void related_posts::flush()
{
  if (posts.size() > 0) {
    foreach (post_t * post, posts) {
      if (post->xact) {
	foreach (post_t * r_post, post->xact->posts) {
	  post_t::xdata_t& xdata(r_post->xdata());
	  if (! xdata.has_flags(POST_EXT_HANDLED) &&
	      (! xdata.has_flags(POST_EXT_RECEIVED) ?
	       ! r_post->has_flags(ITEM_GENERATED | POST_VIRTUAL) :
	       also_matching)) {
	    xdata.add_flags(POST_EXT_HANDLED);
	    item_handler<post_t>::operator()(*r_post);
	  }
	}
      } else {
	// This code should only be reachable from the "output"
	// command, since that is the only command which attempts to
	// output auto or period xacts.
	post_t::xdata_t& xdata(post->xdata());
	if (! xdata.has_flags(POST_EXT_HANDLED) &&
	    ! post->has_flags(ITEM_GENERATED)) {
	  xdata.add_flags(POST_EXT_HANDLED);
	  item_handler<post_t>::operator()(*post);
	}
      }
    }
  }

  item_handler<post_t>::flush();
}

void changed_value_posts::output_revaluation(post_t * post, const date_t& date)
{
  if (is_valid(date))
    post->xdata().date = date;

  value_t repriced_total;
  try {
    bind_scope_t bound_scope(report, *post);
    repriced_total = total_expr.calc(bound_scope);
  }
  catch (...) {
    post->xdata().date = date_t();
    throw;
  }
  post->xdata().date = date_t();

  DEBUG("filter.changed_value",
	"output_revaluation(last_balance) = " << last_total);
  DEBUG("filter.changed_value",
	"output_revaluation(repriced_total) = " << repriced_total);

  if (! last_total.is_null()) {
    if (value_t diff = repriced_total - last_total) {
      DEBUG("filter.changed_value", "output_revaluation(strip(diff)) = "
	    << diff.strip_annotations(report.what_to_keep()));

      xact_temps.push_back(xact_t());
      xact_t& xact = xact_temps.back();
      xact.payee = _("Commodities revalued");
      xact._date = is_valid(date) ? date : post->date();

      handle_value(diff, &revalued_account, &xact, post_temps, *handler,
		   *xact._date, repriced_total, false,
		   optional<post_functor_t>
		   (bind(&changed_value_posts::output_rounding, this, _1)));
    }
  }
}

void changed_value_posts::output_rounding(post_t * post)
{
  bind_scope_t bound_scope(report, *post);
  value_t      new_display_total(display_total_expr.calc(bound_scope));

  DEBUG("filter.changed_value.rounding",
	"rounding.new_display_total     = " << new_display_total);

  if (! last_display_total.is_null()) {
    if (value_t repriced_amount = display_amount_expr.calc(bound_scope)) {
      DEBUG("filter.changed_value.rounding",
	    "rounding.repriced_amount       = " << repriced_amount);

      value_t precise_display_total(new_display_total.truncated() -
				    repriced_amount.truncated());

      DEBUG("filter.changed_value.rounding",
	    "rounding.precise_display_total = " << precise_display_total);
      DEBUG("filter.changed_value.rounding",
	    "rounding.last_display_total    = " << last_display_total);

      if (value_t diff = precise_display_total - last_display_total) {
	DEBUG("filter.changed_value.rounding",
	      "rounding.diff                  = " << diff);

	xact_temps.push_back(xact_t());
	xact_t& xact = xact_temps.back();
	xact.payee = _("Commodity rounding");
	xact._date = post->date();

	handle_value(diff, &rounding_account, &xact, post_temps,
		     *handler, *xact._date, precise_display_total, true);
      }
    }    
  }
  last_display_total = new_display_total;
}

void changed_value_posts::operator()(post_t& post)
{
  if (last_post)
    output_revaluation(last_post, post.date());

  if (changed_values_only)
    post.xdata().add_flags(POST_EXT_DISPLAYED);

  output_rounding(&post);

  item_handler<post_t>::operator()(post);

  bind_scope_t bound_scope(report, post);
  last_total = total_expr.calc(bound_scope);

  last_post = &post;
}

void subtotal_posts::report_subtotal(const char *		      spec_fmt,
				     const optional<date_interval_t>& interval)
{
  if (component_posts.empty())
    return;

  optional<date_t> range_start  = interval ? interval->start : none;
  optional<date_t> range_finish = interval ? interval->inclusive_end() : none;

  foreach (post_t * post, component_posts) {
    date_t date = post->date();
    if (! range_start || date < *range_start)
      range_start = date;
    if (! range_finish || date > *range_finish)
      range_finish = date;
  }
  component_posts.clear();

  std::ostringstream out_date;
  if (spec_fmt) {
    out_date << format_date(*range_finish, string(spec_fmt));
  }
  else if (date_format) {
    string fmt = "- ";
    fmt += *date_format;
    out_date << format_date(*range_finish, string(fmt));
  }
  else {
    out_date << format_date(*range_finish, std::string("- ") + output_date_format);
  }

  xact_temps.push_back(xact_t());
  xact_t& xact = xact_temps.back();
  xact.payee = out_date.str();
  xact._date = *range_start;

  foreach (values_map::value_type& pair, values)
    handle_value(pair.second.value, pair.second.account, &xact, post_temps,
		 *handler);

  values.clear();
}

void subtotal_posts::operator()(post_t& post)
{
  component_posts.push_back(&post);

  account_t * acct = post.reported_account();
  assert(acct);

  values_map::iterator i = values.find(acct->fullname());
  if (i == values.end()) {
    value_t temp;
    post.add_to_value(temp, amount_expr);
    std::pair<values_map::iterator, bool> result
      = values.insert(values_pair(acct->fullname(), acct_value_t(acct, temp)));
    assert(result.second);
  } else {
    post.add_to_value((*i).second.value, amount_expr);
  }

  // If the account for this post is all virtual, mark it as
  // such, so that `handle_value' can show "(Account)" for accounts
  // that contain only virtual posts.

  post.reported_account()->xdata().add_flags(ACCOUNT_EXT_AUTO_VIRTUALIZE);

  if (! post.has_flags(POST_VIRTUAL))
    post.reported_account()->xdata().add_flags(ACCOUNT_EXT_HAS_NON_VIRTUALS);
  else if (! post.has_flags(POST_MUST_BALANCE))
    post.reported_account()->xdata().add_flags(ACCOUNT_EXT_HAS_UNB_VIRTUALS);
}

void interval_posts::report_subtotal(const date_interval_t& interval)
{
  if (last_post && interval) {
    if (exact_periods)
      subtotal_posts::report_subtotal();
    else
      subtotal_posts::report_subtotal(NULL, interval);
  }

  last_post = NULL;
}

void interval_posts::operator()(post_t& post)
{
  date_t date = post.date();

  if (! interval.find_period(post.date()))
    return;

  if (interval.duration) {
    if (last_interval && interval != last_interval) {
      report_subtotal(last_interval);

      if (generate_empty_posts) {
	for (++last_interval; interval != last_interval; ++last_interval) {
	  // Generate a null posting, so the intervening periods can be
	  // seen when -E is used, or if the calculated amount ends up being
	  // non-zero
	  xact_temps.push_back(xact_t());
	  xact_t& null_xact = xact_temps.back();
	  null_xact.add_flags(ITEM_TEMP);
	  null_xact._date = last_interval.inclusive_end();

	  post_temps.push_back(post_t(&empty_account));
	  post_t& null_post = post_temps.back();
	  null_post.add_flags(ITEM_TEMP | POST_CALCULATED);
	  null_post.amount = 0L;
	  null_xact.add_post(&null_post);

	  last_post = &null_post;
	  subtotal_posts::operator()(null_post);

	  report_subtotal(last_interval);
	}
	assert(interval == last_interval);
      } else {
	last_interval = interval;
      }
    } else {
      last_interval = interval;
    }
    subtotal_posts::operator()(post);
  } else {
    item_handler<post_t>::operator()(post);
  }

  last_post = &post;
}

void posts_as_equity::report_subtotal()
{
  date_t finish;
  foreach (post_t * post, component_posts) {
    date_t date = post->date();
    if (! is_valid(finish) || date > finish)
      finish = date;
  }
  component_posts.clear();

  xact_temps.push_back(xact_t());
  xact_t& xact = xact_temps.back();
  xact.payee = _("Opening Balances");
  xact._date = finish;

  value_t total = 0L;
  foreach (values_map::value_type& pair, values) {
    handle_value(pair.second.value, pair.second.account, &xact, post_temps,
		 *handler);
    total += pair.second.value;
  }
  values.clear();

  if (total.is_balance()) {
    foreach (balance_t::amounts_map::value_type pair,
	     total.as_balance().amounts) {
      post_temps.push_back(post_t(balance_account));
      post_t& balance_post = post_temps.back();
      balance_post.add_flags(ITEM_TEMP);
      balance_post.amount = - pair.second;
      xact.add_post(&balance_post);
      (*handler)(balance_post);
    }
  } else {
    post_temps.push_back(post_t(balance_account));
    post_t& balance_post = post_temps.back();
    balance_post.add_flags(ITEM_TEMP);
    balance_post.amount = - total.to_amount();
    xact.add_post(&balance_post);
    (*handler)(balance_post);
  }
}

void by_payee_posts::flush()
{
  foreach (payee_subtotals_map::value_type& pair, payee_subtotals)
    pair.second->report_subtotal(pair.first.c_str());

  item_handler<post_t>::flush();

  payee_subtotals.clear();
}

void by_payee_posts::operator()(post_t& post)
{
  payee_subtotals_map::iterator i = payee_subtotals.find(post.xact->payee);
  if (i == payee_subtotals.end()) {
    payee_subtotals_pair
      temp(post.xact->payee,
	   shared_ptr<subtotal_posts>(new subtotal_posts(handler, amount_expr)));
    std::pair<payee_subtotals_map::iterator, bool> result
      = payee_subtotals.insert(temp);

    assert(result.second);
    if (! result.second)
      return;
    i = result.first;
  }

  (*(*i).second)(post);
}

void transfer_details::operator()(post_t& post)
{
  xact_temps.push_back(*post.xact);
  xact_t& xact = xact_temps.back();
  xact._date = post.date();

  post_temps.push_back(post);
  post_t& temp = post_temps.back();
  temp.xact = &xact;
  temp.set_state(post.state());
  temp.add_flags(ITEM_TEMP);
  xact.add_post(&temp);

  bind_scope_t bound_scope(scope, temp);

  switch (which_element) {
  case SET_PAYEE:
    xact.payee = expr.calc(bound_scope).to_string();
    break;
  case SET_ACCOUNT:
    temp.account = master->find_account(expr.calc(bound_scope).to_string());
    break;
  default:
    assert(false);
    break;
  }

  item_handler<post_t>::operator()(temp);
}

void dow_posts::flush()
{
  for (int i = 0; i < 7; i++) {
    foreach (post_t * post, days_of_the_week[i])
      subtotal_posts::operator()(*post);
    subtotal_posts::report_subtotal("%As");
    days_of_the_week[i].clear();
  }

  subtotal_posts::flush();
}

void generate_posts::add_period_xacts(period_xacts_list& period_xacts)
{
  foreach (period_xact_t * xact, period_xacts)
    foreach (post_t * post, xact->posts)
      add_post(xact->period, *post);
}

void generate_posts::add_post(const date_interval_t& period, post_t& post)
{
  pending_posts.push_back(pending_posts_pair(period, &post));
}

void budget_posts::report_budget_items(const date_t& date)
{
  if (pending_posts.size() == 0)
    return;

  bool reported;
  do {
    reported = false;
    foreach (pending_posts_list::value_type& pair, pending_posts) {
      optional<date_t> begin = pair.first.start;
      if (! begin) {
	if (! pair.first.find_period(date))
	  throw_(std::runtime_error, _()"Something odd has happened");
	begin = pair.first.start;
      }
      assert(begin);

      if (*begin < date &&
	  (! pair.first.end || *begin < *pair.first.end)) {
	post_t& post = *pair.second;

	DEBUG("ledger.walk.budget", "Reporting budget for "
	      << post.reported_account()->fullname());

	xact_temps.push_back(xact_t());
	xact_t& xact = xact_temps.back();
	xact.payee = _("Budget transaction");
	xact._date = begin;

	post_temps.push_back(post);
	post_t& temp = post_temps.back();
	temp.xact = &xact;
	temp.add_flags(ITEM_TEMP);
	temp.amount.in_place_negate();
	xact.add_post(&temp);

	++pair.first;
	begin = *pair.first.start;

	item_handler<post_t>::operator()(temp);

	reported = true;
      }
    }
  } while (reported);
}

void budget_posts::operator()(post_t& post)
{
  bool post_in_budget = false;

  foreach (pending_posts_list::value_type& pair, pending_posts) {
    for (account_t * acct = post.reported_account();
	 acct;
	 acct = acct->parent) {
      if (acct == (*pair.second).reported_account()) {
	post_in_budget = true;
	// Report the post as if it had occurred in the parent
	// account.
	if (post.reported_account() != acct)
	  post.xdata().account = acct;
	goto handle;
      }
    }
  }

 handle:
  if (post_in_budget && flags & BUDGET_BUDGETED) {
    report_budget_items(post.date());
    item_handler<post_t>::operator()(post);
  }
  else if (! post_in_budget && flags & BUDGET_UNBUDGETED) {
    item_handler<post_t>::operator()(post);
  }
}

void forecast_posts::add_post(const date_interval_t& period, post_t& post)
{
  generate_posts::add_post(period, post);

  date_interval_t& i = pending_posts.back().first;
  if (! i.start) {
    if (! i.find_period(CURRENT_DATE()))
      throw_(std::runtime_error, _("Something odd has happened"));
    ++i;
  } else {
    while (*i.start < CURRENT_DATE())
      ++i;
  }
}

void forecast_posts::flush()
{
  posts_list passed;
  date_t     last;

  while (pending_posts.size() > 0) {
    pending_posts_list::iterator least = pending_posts.begin();
    for (pending_posts_list::iterator i = ++pending_posts.begin();
	 i != pending_posts.end();
	 i++)
      if (*(*i).first.start < *(*least).first.start)
	least = i;

    date_t& begin = *(*least).first.start;

    if ((*least).first.end && begin >= *(*least).first.end) {
      pending_posts.erase(least);
      passed.remove((*least).second);
      continue;
    }

    post_t& post = *(*least).second;

    xact_temps.push_back(xact_t());
    xact_t& xact = xact_temps.back();
    xact.payee = _("Forecast transaction");
    xact._date = begin;

    post_temps.push_back(post);
    post_t& temp = post_temps.back();
    temp.xact = &xact;
    temp.add_flags(ITEM_TEMP);
    xact.add_post(&temp);

    date_t next = *(*least).first.next;
    ++(*least).first;

    if (next < begin || (is_valid(last) && (next - last).days() > 365 * 5))
      break;
    begin = next;

    item_handler<post_t>::operator()(temp);

    if (temp.has_xdata() &&
	temp.xdata().has_flags(POST_EXT_MATCHES)) {
      bind_scope_t bound_scope(context, temp);
      if (! pred(bound_scope))
	break;
      last = temp.date();
      passed.clear();
    } else {
      bool found = false;
      foreach (post_t * x, passed)
	if (x == &post) {
	  found = true;
	  break;
	}

      if (! found) {
	passed.push_back(&post);
	if (passed.size() >= pending_posts.size())
	  break;
      }
    }
  }

  item_handler<post_t>::flush();
}

pass_down_accounts::pass_down_accounts(acct_handler_ptr		       handler,
				       accounts_iterator&	       iter,
				       const optional<item_predicate>& _pred,
				       const optional<scope_t&>&       _context)
  : item_handler<account_t>(handler), pred(_pred), context(_context)
{
  TRACE_CTOR(pass_down_accounts, "acct_handler_ptr, accounts_iterator, ...");

  for (account_t * account = iter(); account; account = iter()) {
    if (! pred) {
      item_handler<account_t>::operator()(*account);
    } else {
      bind_scope_t bound_scope(*context, *account);
      if ((*pred)(bound_scope))
	item_handler<account_t>::operator()(*account);
    }
  }

  item_handler<account_t>::flush();
}

} // namespace ledger
