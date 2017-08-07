/*
 * Copyright (c) 2017, Respective Authors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <eos/chain/block_schedule.hpp>
#include <eos/chain/block.hpp>

namespace eos { namespace chain {

static uint next_power_of_two(uint input) {
   if (input == 0) {
      return 0;
   }

   uint result = input;
   result--;
   result |= result >> 1;
   result |= result >> 2;
   result |= result >> 4;
   result |= result >> 8;
   result |= result >> 16;
   result++;
   return result;
};

typedef std::hash<decltype(AccountName::value)> account_hash;
static account_hash account_hasher;

struct schedule_entry {
   schedule_entry(uint _cycle, uint _thread, pending_transaction const * _transaction) 
      : cycle(_cycle)
      , thread(_thread)
      , transaction(_transaction) 
   {}

   uint cycle;
   uint thread;
   pending_transaction const *transaction;

   friend bool operator<( schedule_entry const &l, schedule_entry const &r ) {
      if (l.cycle < r.cycle) {
            return true;
      } else if (l.cycle == r.cycle) {
            return l.thread < r.thread;
      } 

      return false;
   }
};

static block_schedule from_entries(vector<schedule_entry> &entries) {
   // sort in reverse to save allocations, this should put the highest thread index
   // for the highest cycle index first meaning the naive resize in the loop below
   // is usually the largest and only resize
   auto reverse = [](schedule_entry const &l, schedule_entry const &r) {
      return !(l < r);
   };

   std::sort(entries.begin(), entries.end(), reverse);

   block_schedule result;
   for(auto const & entry : entries) {
      if (result.cycles.size() <= entry.cycle) {
         result.cycles.resize(entry.cycle + 1);
      }

      auto &cycle = result.cycles.at(entry.cycle);

      if (cycle.size() <= entry.thread) {
         cycle.resize(entry.thread + 1);
      }

      // because we are traversing the schedule in reverse to save
      // allocations, we cannot emplace_back as that would reverse
      // the transactions in a thread
      auto &thread = cycle.at(entry.thread);
      thread.transactions.emplace(thread.transactions.begin(), entry.transaction);
   }

   return result;
}

template<typename CONTAINER>
auto initialize_pointer_vector(CONTAINER const &c) {
   vector<pending_transaction const *> result;
   result.reserve(c.size());
   for (auto const &t : c) {
      result.emplace_back(&t);
   }
   
   return result;
}

struct transaction_size_visitor : public fc::visitor<size_t>
{
   template <typename T>
   size_t operator()(const T &trx_p) const {
      return fc::raw::pack_size(*trx_p);
   }
};

struct block_size_skipper {
   size_t current_size;
   size_t const max_size;

   bool should_skip(pending_transaction const *t) const {
      size_t transaction_size = t->visit(transaction_size_visitor());
      // postpone transaction if it would make block too big
      if( transaction_size + current_size > max_size ) {
         return true;
      } else {
         return false;
      }
   }

   void apply(pending_transaction const *t) {
      size_t transaction_size =  t->visit(transaction_size_visitor());
      current_size += transaction_size;
   }
};

auto make_skipper(const global_property_object &properties) {
   static const size_t max_block_header_size = fc::raw::pack_size( signed_block_header() ) + 4;
   auto maximum_block_size = properties.configuration.maxBlockSize;
   return block_size_skipper { max_block_header_size, (size_t)maximum_block_size };
}

block_schedule block_schedule::by_threading_conflicts(
   vector<pending_transaction> const &transactions,
   const global_property_object &properties
   )
{
   static uint const MAX_TXS_PER_THREAD = 4;
   auto skipper = make_skipper(properties);

   uint HASH_SIZE = std::max<uint>(4096, next_power_of_two(transactions.size() / 8));
   vector<optional<uint>> assigned_threads(HASH_SIZE);

   vector<schedule_entry> schedule;
   schedule.reserve(transactions.size());

   auto current = initialize_pointer_vector(transactions);
   vector<pending_transaction const *> postponed;
   postponed.reserve(transactions.size());

   vector<uint> txs_per_thread;
   txs_per_thread.reserve(HASH_SIZE);

   int cycle = 0;
   bool scheduled = true;
   while (scheduled) {
      scheduled = false;
      uint next_thread = 0;
      
      for (auto t : current) {
         // skip ?
         if (skipper.should_skip(t)) {
            continue;
         }

         auto assigned_to = optional<uint>();
         bool postpone = false;
         auto scopes = t->visit(scope_extracting_visitor());

         for (const auto &a : scopes) {
            uint hash_index = account_hasher(a) % HASH_SIZE;
            if (assigned_to && assigned_threads[hash_index] && assigned_to != assigned_threads[hash_index]) {
               postpone = true;
               postponed.push_back(t);
               break;
            }

            if (assigned_threads[hash_index])
            {
               assigned_to = assigned_threads[hash_index];
            }
         }

         if (!postpone) {
            if (!assigned_to) {
               assigned_to = next_thread++;
               txs_per_thread.resize(next_thread);
            }

            if (txs_per_thread[*assigned_to] < MAX_TXS_PER_THREAD) {
               for (const auto &a : scopes)
               {
                  uint hash_index = account_hasher(a) % HASH_SIZE;
                  assigned_threads[hash_index] = assigned_to;
               }

               txs_per_thread[*assigned_to]++;
               schedule.emplace_back(cycle, *assigned_to, t);
               scheduled = true;
               skipper.apply(t);
            } else {
               postponed.push_back(t);
            }
         }
      }
      
      current.resize(0);
      txs_per_thread.resize(0);
      assigned_threads.resize(0);
      assigned_threads.resize(HASH_SIZE);
      std::swap(current, postponed);
      ++cycle;

   }
   return from_entries(schedule);
}

block_schedule block_schedule::by_cycling_conflicts(
    vector<pending_transaction> const &transactions,
    const global_property_object &properties
    )
{
   auto skipper = make_skipper(properties);

   uint HASH_SIZE = std::max<uint>(4096, next_power_of_two(transactions.size() / 8));
   vector<schedule_entry> schedule;
   schedule.reserve(transactions.size());

   auto current = initialize_pointer_vector(transactions);
   vector<pending_transaction const *> postponed;
   postponed.reserve(transactions.size());

   int cycle = 0;
   vector<bool> used(HASH_SIZE);
   bool scheduled = true;
   while (scheduled) {
      scheduled = false;
      int thread = 0;
      for (auto t : current) {
         // skip ?
         if (skipper.should_skip(t)) {
            continue;
         }

         auto scopes = t->visit(scope_extracting_visitor());
         bool u = false;
         for (const auto &a : scopes) {
            uint hash_index = account_hasher(a) % HASH_SIZE;
            if (used[hash_index]) {
               u = true;
               postponed.push_back(t);
               break;
            }
         }

         if (!u) {
            for (const auto &a : scopes) {
               uint hash_index = account_hasher(a) % HASH_SIZE;
               used[hash_index] = true;
            }

            schedule.emplace_back(cycle, thread++, t);
            scheduled = true;
            skipper.apply(t);
         }
      }

      current.resize(0);
      used.resize(0);
      used.resize(HASH_SIZE);
      std::swap(current, postponed);
      ++cycle;
   }

   return from_entries(schedule);
}

} /* namespace chain */ } /* namespace eos */
